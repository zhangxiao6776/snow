#include "Particle.h"

Particle::Particle(){}
Particle::Particle(const Vector2f& pos, const Vector2f& vel, float mass, float lame_lambda, float lame_mu){
	position.setData(pos);
	velocity.setData(vel);
	this->mass = mass;
	lambda_s = lame_lambda;
	mu_s = lame_mu;
	//To start out with, we assume the deformation gradient is zero
	//Or in other words, all particle velocities are the same
	def_elastic.loadIdentity();
	def_plastic.loadIdentity();
	det_elastic = 1;
	det_plastic = 1;
	svd_e.setData(1, 1);
	svd_w.loadIdentity();
	svd_v.loadIdentity();
	polar_r.loadIdentity();
	polar_s.loadIdentity();
}
Particle::~Particle(){}

void Particle::updatePos(){
	//Simple euler integration
	position += TIMESTEP*velocity;
}
void Particle::updateGradient(){
	//So, initially we make all updates elastic
	velocity_gradient *= TIMESTEP;
	velocity_gradient.diag_sum(1);
	def_elastic.setData(velocity_gradient * def_elastic);
	Matrix2f f_all = def_elastic * def_plastic;
	
	//We compute the SVD decomposition
	//The singular values (basically a scale transform) tell us if 
	//the particle has exceeded critical stretch/compression
	def_elastic.svd(&svd_w, &svd_e, &svd_v);
	Matrix2f svd_v_trans = svd_v.transpose();
	//Clamp singular values to within elastic region
	for (int i=0; i<2; i++){
		if (svd_e[i] < CRIT_COMPRESS)
			svd_e[i] = CRIT_COMPRESS;
		else if (svd_e[i] > CRIT_STRETCH)
			svd_e[i] = CRIT_STRETCH;
	}
	//Compute polar decomposition, from clamped SVD
	polar_r.setData(svd_w*svd_v_trans);
	polar_s.setData(svd_v);
	polar_s.diag_product(svd_e);
	polar_s.setData(polar_s*svd_v_trans);
	
	//Recompute elastic and plastic gradient
	//We're basically just putting the SVD back together again
	Matrix2f v_cpy(svd_v), w_cpy(svd_w);
	v_cpy.diag_product_inv(svd_e);
	w_cpy.diag_product(svd_e);
	def_plastic = v_cpy*svd_w.transpose()*f_all;
	def_elastic = w_cpy*svd_v.transpose();
	
	//Update lame parameters to account for hardening
	det_elastic = def_elastic.determinant();
	det_plastic = def_plastic.determinant();
	float scale = exp(HARDENING*(1-det_plastic));
	mu = mu_s*scale;
	lambda = lambda_s*scale;
}
const Matrix2f Particle::cauchyStress(){
	/* Stress force on each particle is: -volume*cauchy_stress
		We transfer the force to the FEM grid using the shape function gradient
		cauchy_stress can be computed via: (2u(Fe - Re)*Fe^T + y(Je - 1)Je*I)/J
			I: identity matrix
			Fe: elastic deformation gradient
			Fp: plastic deformation gradient
			Re: rotation term of Fe's polar decomposition
				equivalent to WV* from SVD decomposition
			J: determinant of Fe*Fp
			Je: determinant of Fe
			u/y: Lame parameters

		Note: volume = volume_initial*J, so the J drops out
	*/
	
	Matrix2f temp = def_elastic - polar_r;
	temp *= 2*mu;
	Matrix2f temp2 = temp*def_elastic.transpose();
	temp2.diag_sum(lambda*det_elastic*(det_elastic-1));
	return volume * temp2;
}
const Vector2f Particle::deltaForce(const Vector2f& u, const Vector2f& weight_grad){
	//For detailed explanation, check out the implicit math pdf for details
	//Before we do the force calculation, we need deltaF, deltaR, and delta(JF^-T)
	
	//Finds delta(Fe), where Fe is the elastic deformation gradient
	//Probably can optimize this expression with parentheses...
	Matrix2f del_elastic = TIMESTEP*u.dyadic_product(weight_grad)*def_elastic;
	
	//Check to make sure we should do these calculations?
	if (del_elastic[0][0] < MATRIX_EPSILON && del_elastic[0][1] < MATRIX_EPSILON &&
		del_elastic[1][0] < MATRIX_EPSILON && del_elastic[1][1] < MATRIX_EPSILON)
		return Vector2f(0);

	//Compute R^T*dF - dF^TR
	//It is skew symmetric, so we only need to compute one value (three for 3D)
	float y = (polar_r[0][0]*del_elastic[1][0] + polar_r[1][0]*del_elastic[1][1]) -
				(polar_r[0][1]*del_elastic[0][0] + polar_r[1][1]*del_elastic[0][1]);
	//Next we need to compute MS + SM, where S is the hermitian matrix (symmetric for real
	//valued matrices) of the polar decomposition and M is (R^T*dR); This is equal
	//to the matrix we just found (R^T*dF ...), so we set them equal to eachother
	//Since everything is so symmetric, we get a nice system of linear equations
	//once we multiply everything out. (see pdf for details)
	//In the case of 2D, we only need to solve for one variable (three for 3D)
	float x = y / (polar_s[0][0] + polar_s[1][1]);
	//Final computation is deltaR = R*(R^T*dR)
	Matrix2f del_rotate = Matrix2f(
		-polar_r[1][0]*x, polar_r[0][0]*x,
		-polar_r[1][1]*x, polar_r[0][1]*x
	);
	
	//We need the cofactor matrix of F, JF^-T
	Matrix2f cofactor = def_elastic.cofactor();
		
	//The last matrix we need is delta(JF^-T)
	//Instead of doing the complicated matrix derivative described in the paper
	//we can just take the derivative of each individual entry in JF^-T; JF^-T is
	//the cofactor matrix of F, so we'll just hardcode the whole thing
	//For example, entry [0][0] for a 3x3 matrix is
	//	cofactor = e*i - f*h
	//	derivative = (e*Di + De*i) - (f*Dh + Df*h)
	//	where the derivatives (capital D) come from our precomputed delta(F)
	//In the case of 2D, this turns out to be just the cofactor of delta(F)
	//For 3D, it will not be so simple
	Matrix2f del_cofactor = del_elastic.cofactor();

	//Calculate "A" as given by the paper
	//Co-rotational term
	Matrix2f Ap = del_elastic-del_rotate;
	Ap *= 2*mu;
	//Primary contour term
	cofactor *= cofactor.frobeniusInnerProduct(del_elastic);
	del_cofactor *= (det_elastic-1);
	cofactor += del_cofactor;
	cofactor *= lambda;
	Ap += cofactor;
	
	//Put it all together
	//Parentheses are important; M*M*V is slower than M*(M*V)
	return volume*(Ap*(def_elastic.transpose()*weight_grad));
}