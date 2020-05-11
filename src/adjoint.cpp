/*@ ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 **
 **    Copyright (c) 2011-2015, JGU Mainz, Anton Popov, Boris Kaus
 **    All rights reserved.
 **
 **    This software was developed at:
 **
 **         Institute of Geosciences
 **         Johannes-Gutenberg University, Mainz
 **         Johann-Joachim-Becherweg 21
 **         55128 Mainz, Germany
 **
 **    project:    LaMEM
 **    filename:   adjoint.c
 **
 **    LaMEM is free software: you can redistribute it and/or modify
 **    it under the terms of the GNU General Public License as published
 **    by the Free Software Foundation, version 3 of the License.
 **
 **    LaMEM is distributed in the hope that it will be useful,
 **    but WITHOUT ANY WARRANTY; without even the implied warranty of
 **    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 **    See the GNU General Public License for more details.
 **
 **    You should have received a copy of the GNU General Public License
 **    along with LaMEM. If not, see <http://www.gnu.org/licenses/>.
 **
 **
 **    Contact:
 **        Boris Kaus       [kaus@uni-mainz.de]
 **        Anton Popov      [popov@uni-mainz.de]
 **
 **
 **    Main development team:
 **         Anton Popov      [popov@uni-mainz.de]
 **         Boris Kaus       [kaus@uni-mainz.de]
 **         Tobias Baumann
 **         Adina Pusok
 **         Arthur Bauville
 **
 ** ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ @*/

// FRAMEWORK CODE FOR LaMEM TO USE ADJOINT GRADIENT (INVERSION)
// *  developed by Georg Reuber (JGU Mainz)
// *  publication: Georg S. Reuber, Anton A. Popov, Boris J.P. Kaus, Deriving scaling laws in geodynamics using adjoint gradients, Tectonophysics, 2017
//---------------------------------------------------------------------------
// COMPUTATION OF ADJOINT INVERSION
//---------------------------------------------------------------------------
// RECIPE:
// Objective function    F(x,x(p)) = (1/2)*[P*(x-x_ini)' * P*(x-x_ini)]      // p = parameter ; x = converged solution ; xini = comparison solution (same size as jr->gsol) ; P = Projection vector containing the proportions of solution influence
// Derivative I          dF/dx     = P*x-P*x_ini
// Adjoint operation     psi       = (J^T)^-1 * dF/dx           // J = converged Jacobain matrix
// Derivative II         dr/dp     = [r(p+h) - r(p)]/h      // finite difference approximation of derivative of residual r vs parameter
// Gradients             dF/dp     = -psi^T * dr/dp
//
// ------------------------------------------------------------
// EXAMPLE IN INPUT FILE:
//  # General
//	Inv_use       = 2
//  Inv_Ap        = 1
//  Inv_OFdef     = 1
//  Inv_Tao       = 1
//  # Parameters
//  <InverseParStart>
//	   	Inv_ID  = 0
//		Inv_Typ = rho0
//		Inv_Par = 1
//	<InverseParEnd>
//  # Index
//	<InverseIndStart>
//		Inv_Ax = 4.95;
//		Inv_Ay = 0.05;
//		Inv_Az = 0.68;
//		Inv_Av = 3;
//		Inv_Ae = 1;
//	<InverseIndEnd>
//
// ------------------------------------------------------------
// LINEAR SOLVER:
// You can control the behaviour of the KSP object for the adjoint with the prefix "as_" (the same way as "js_")
//
// ------------------------------------------------------------
// FULL INVERSION REMARKS:
// 1) In case you want to perform the full adjoint inversion (use = 3)  and you do not want to specify comparison points by hand (OFdef = 1) make sure that you have a
//    comparison file with a petsc vector the same size as jr->gsol and called Forward_Solution.bin. Most likely you want to run a forward simulation and then change
//    the parameters to do so just run your forward model with use = 4 which automatically saves this file. Perturb the values within the input script and solve again
//    with use = 3.
//
// 2) You can control the behaviour of the TAO object for the adjoint with the prefix "tao_" (example: '-tao_type lmvm' ; '-tao_fatol 1e-15' ; '-tao_converged_reason')
//
// 3) In case you want to use powerlaw viscosities make sure the reference strainrate is the same as the one that you use in the viscosity computation!!
//
// ------------------------------------------------------------
// IMPORTANT REMARKS:
// 1) Since the Adjoint needs the Jacobian matrix for computing the gradients it's crucial to make sure that
//    you compute the Jacobian matrix in the timesteps where you want to compute the gradients
//    (f.e. a linear problem would need a low value for -snes_atol [1e-20] and a low max iteration count -snes_max_it [2] to
//    guarantee the computation of the Jacobian + the option '-snes_type ksponly')
// 2) This code does not actually solve the system with the transposed Jacobian but uses the original Jacobian as approximation. So you should make
//    sure that your Jacobian is symmteric (e.g. use lithostatic pressure in plasticity, etc..)
//---------------------------------------------------------------------------
#include "LaMEM.h"
#include "adjoint.h"
#include "phase.h"
#include "tools.h"
#include "fdstag.h"
#include "scaling.h"
#include "tssolve.h"
#include "bc.h"
#include "JacRes.h"
#include "interpolate.h"
#include "surf.h"
#include "multigrid.h"
#include "matrix.h"
#include "lsolve.h"
#include "nlsolve.h"
#include "objFunct.h"
#include "constEq.h"
#include "parsing.h"
#include "gravity.h"
//-----------------------------------------------------------------------------
// A bit stupid that this has to be twice declared, but the original function is only in AVD.cpp and not in a header file anymore ...
PetscInt FindPointInCellAdjoint(
	PetscScalar *px, // node coordinates
	PetscInt     L,  // index of the leftmost node
	PetscInt     R,  // index of the rightmost node
	PetscScalar  x)  // point coordinate
{
	// find ID of the cell containing point (call this function for local point only!)
	if(x < px[L] || x > px[R])
	{
		SETERRQ(PETSC_COMM_SELF, PETSC_ERR_USER, "Non-local marker");
	}
	// get initial guess assuming uniform grid
	PetscInt M = L + (PetscInt)((x-px[L])/((px[R]-px[L])/(PetscScalar)(R-L)));

	if(M == R) return R-1;

	if(px[M]   <= x) L=M;
	if(px[M+1] >= x) R=M+1;

	while((R-L) > 1)
	{
		M = (L+R)/2;
		if(px[M] <= x) L=M;
		if(px[M] >= x) R=M;

	}
	return(L);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "LaMEMAdjointMain"
PetscErrorCode LaMEMAdjointMain(ModParam *IOparam, FB *fb)
{
	PetscErrorCode ierr;
	PetscFunctionBegin;

	Scaling         scal;
	PetscScalar    *gradar, *Ubar, *Lbar, *Par, *fcconvar, F, ts;
	Vec             val, Ub, Lb, grad, P;
	PetscInt        i, ti;
	char            str[_str_len_];

	IOparam->count = 1;  // iteration counter for the initial cost function
	F              = 1e100;

	// Some defaults
	IOparam->FS         = 0;
	IOparam->Gr         = 1;
	IOparam->mdI        = 0;
	IOparam->Ab         = 0;
	IOparam->Ap         = 2;
	IOparam->Adv        = 0;
	IOparam->OFdef      = 0;
	IOparam->Tao        = 1;
	IOparam->tol        = 1e-10;
	IOparam->facLS      = 2;
	IOparam->facB       = 0.5;
	IOparam->maxfac     = 100;
	IOparam->Scale_Grad = 0.1;
	IOparam->maxit      = 50;
	IOparam->maxitLS    = 10;

	// Some inputs
	ierr = getIntParam   (fb, _OPTIONAL_, "Adj_FS"        , &IOparam->FS,        1, 1        ); CHKERRQ(ierr);  // Do a field sensitivity test? -> Will do the test for the first InverseParStart that is given!
	ierr = getIntParam   (fb, _OPTIONAL_, "Inv_maxit"     , &IOparam->maxit,     1, 1500     ); CHKERRQ(ierr);  // maximum number of inverse iterations
	ierr = getIntParam   (fb, _OPTIONAL_, "Inv_maxitLS"   , &IOparam->maxitLS,   1, 1500     ); CHKERRQ(ierr);  // maximum number of backtracking	
	ierr = getIntParam   (fb, _OPTIONAL_, "Adj_Gr"        , &IOparam->Gr,        1, 1        ); CHKERRQ(ierr);  // Calculate Grads with respect to solution (scaling laws etc.) = 1; Calc with respect to cost function (direct fD test) = 0
	ierr = getIntParam   (fb, _OPTIONAL_, "Inv_Ab"        , &IOparam->Ab,        1, 1        ); CHKERRQ(ierr);  // Apply bounds?
	ierr = getIntParam   (fb, _OPTIONAL_, "Adj_Ap"        , &IOparam->Ap,        1, 3        ); CHKERRQ(ierr);  // 1 = several indices ; 2 = the whole domain ; 3 = surface
	ierr = getIntParam   (fb, _OPTIONAL_, "Adj_Adv"       , &IOparam->Adv,       1, 1        ); CHKERRQ(ierr);  // 1 = advect the point
	ierr = getIntParam   (fb, _OPTIONAL_, "Adj_OFdef"     , &IOparam->OFdef,     1, 1        ); CHKERRQ(ierr);  // Objective function defined by hand?
	ierr = getIntParam   (fb, _OPTIONAL_, "Inv_Tao"       , &IOparam->Tao,       1, 1        ); CHKERRQ(ierr);  // Use TAO?
	ierr = getScalarParam(fb, _OPTIONAL_, "Inv_tol"       , &IOparam->tol,       1, 1        ); CHKERRQ(ierr);  // tolerance for F/Fini after which code has converged
	ierr = getScalarParam(fb, _OPTIONAL_, "Inv_facLS"     , &IOparam->facLS,     1, 1        ); CHKERRQ(ierr);  // factor in the line search that multiplies current line search parameter if GD update was succesful (increases convergence speed)
	ierr = getScalarParam(fb, _OPTIONAL_, "Inv_facB"      , &IOparam->facB,      1, 1        ); CHKERRQ(ierr);  // backtrack factor that multiplies current line search parameter if GD update was not succesful
	ierr = getScalarParam(fb, _OPTIONAL_, "Inv_maxfac"    , &IOparam->maxfac,    1, 1        ); CHKERRQ(ierr);  // limit on the factor (only used without tao)
	ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Scale_Grad", &IOparam->Scale_Grad,1, 1        ); CHKERRQ(ierr);  // Magnitude of initial parameter update (factor_ini = Scale_Grad/Grad)
	ierr = getScalarParam(fb, _REQUIRED_, "DII"           , &IOparam->DII_ref,   1, 1        ); CHKERRQ(ierr);   // SUPER UNNECESSARY BUT OTHERWISE NOT AVAILABLE

	PetscPrintf(PETSC_COMM_WORLD,"-------------------------------------------------------------------------- \n");
	PetscPrintf(PETSC_COMM_WORLD,"                        LAMEM                         \n");
	PetscPrintf(PETSC_COMM_WORLD,"        ADJOINT GRADIENT CALCULATION ACTIVE           \n");
	PetscPrintf(PETSC_COMM_WORLD,"-------------------------------------------------------------------------- \n");
	if(IOparam->use == 2) 
	{
		PetscPrintf(PETSC_COMM_WORLD, "   Calculate gradients (use = 2)                    \n");
		PetscPrintf(PETSC_COMM_WORLD, "   Pointwise gradient evaluation            : %d    \n", IOparam->FS);
		PetscPrintf(PETSC_COMM_WORLD, "   Gradients w.r.t.                         : %d  (0 = cost function; 1 = solution)  \n", IOparam->Gr);
		PetscPrintf(PETSC_COMM_WORLD, "   Index definition                         : %d  (1 = several indices; 2 = whole domain; 3 = surface)  \n", IOparam->Ap);
		PetscPrintf(PETSC_COMM_WORLD, "   Advect indices                           : %d    \n", IOparam->Adv);
		PetscPrintf(PETSC_COMM_WORLD, "   Objective function defined in input      : %d    \n", IOparam->OFdef);
	}
	else if(IOparam->use == 3) 
	{
		PetscPrintf(PETSC_COMM_WORLD, "   Gradient descent inversion (use = 3)             \n");
		PetscPrintf(PETSC_COMM_WORLD, "   Use Tao BLMVM (or LaMEM steepest descent): %d    \n", IOparam->Tao);
		PetscPrintf(PETSC_COMM_WORLD, "   Index definition                         : %d  (1 = several indices; 2 = whole domain; 3 = surface)  \n", IOparam->Ap);
		PetscPrintf(PETSC_COMM_WORLD, "   Advect indices                           : %d    \n", IOparam->Adv);
		PetscPrintf(PETSC_COMM_WORLD, "   Objective function defined in input      : %d    \n", IOparam->OFdef);
		PetscPrintf(PETSC_COMM_WORLD, "   Maximum gradient descent iterations      : %d    \n", IOparam->maxit);
		PetscPrintf(PETSC_COMM_WORLD, "   Maximum linesearch iterations            : %d    \n", IOparam->maxitLS);
		PetscPrintf(PETSC_COMM_WORLD, "   Apply bounds                             : %d    \n", IOparam->Ab);
		PetscPrintf(PETSC_COMM_WORLD, "   Tolerance (F/Fini)                       : %.5e  \n", IOparam->tol);
		if (IOparam->Tao == 0)
		{
			PetscPrintf(PETSC_COMM_WORLD, "   Linesearch factor (succesful update)     : %.5e  \n", IOparam->facLS);
			PetscPrintf(PETSC_COMM_WORLD, "   Linesearch factor (overstep)             : %.5e  \n", IOparam->facB);
			PetscPrintf(PETSC_COMM_WORLD, "   Maximum linesearch factor                : %.5e  \n", IOparam->maxfac);
			PetscPrintf(PETSC_COMM_WORLD, "   Scale for initial parameter update       : %.5e  \n", IOparam->Scale_Grad);
		}
	} 
	else if (IOparam->use == 4) 
	{
		PetscPrintf(PETSC_COMM_WORLD,"        Saving synthetic forward simulation (debugging purpose) (use = 4)           \n");
	}
	else
	{
		SETERRQ1(PETSC_COMM_WORLD, PETSC_ERR_USER, "\nUse = %d not known; should be within [0-4]\n",IOparam->use);
	}

	// VECTORS
	ierr = VecCreateMPI(PETSC_COMM_WORLD, _MAX_PAR_  , PETSC_DETERMINE, &Lb);      CHKERRQ(ierr);
	ierr = VecCreateMPI(PETSC_COMM_WORLD, _MAX_PAR_  , PETSC_DETERMINE, &Ub);      CHKERRQ(ierr);
	ierr = VecCreateMPI(PETSC_COMM_WORLD, _MAX_PAR_  , PETSC_DETERMINE, &val);     CHKERRQ(ierr);
	ierr = VecCreateMPI(PETSC_COMM_WORLD, _MAX_PAR_  , PETSC_DETERMINE, &P);       CHKERRQ(ierr);
	ierr = VecCreateMPI(PETSC_COMM_WORLD, _MAX_PAR_  , PETSC_DETERMINE, &grad);    CHKERRQ(ierr);
	ierr = VecCreateMPI(PETSC_COMM_WORLD, IOparam->maxit, PETSC_DETERMINE, &IOparam->fcconv);   CHKERRQ(ierr);   // 1500 is the maximum inversion iterations that are accepted

	// TEMPORARY VARIABLES
	PetscInt		phsar[_MAX_PAR_];
	PetscInt      	typar[_MAX_PAR_];
	PetscScalar     Ax[_MAX_IND_];
	PetscScalar     Ay[_MAX_IND_];
	PetscScalar     Az[_MAX_IND_];
	PetscInt        Av[_MAX_IND_];
	PetscScalar     Ae[_MAX_IND_];

	ierr =  ScalingCreate(&scal, fb);

	// PARAMETERS
	// Get parameter / typ / etc.
	ierr = FBFindBlocks(fb, _OPTIONAL_, "<InverseParStart>", "<InverseParEnd>"); CHKERRQ(ierr);

	// error checking
	if(fb->nblocks > _MAX_PAR_)
	{
		SETERRQ1(PETSC_COMM_WORLD, PETSC_ERR_USER, "Too many inverse parameters specified! Max allowed: %lld", (LLD)_MAX_PAR_);
	}
	if(!fb->nblocks)
	{
		SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER, "You have to define Parameters (mdN) for the inversion. Have a look into the comments in src/LaMEM.cpp");
	}

	// read each individual parameter
	VecGetArray(P,&Par);
	VecGetArray(Ub,&Ubar);
	VecGetArray(Lb,&Lbar);
	VecGetArray(grad,&gradar);

	for(i = 0; i < fb->nblocks; i++)
	{
		ierr = getIntParam   (fb, _OPTIONAL_, "Inv_ID" , &ti, 1, _max_num_phases_); CHKERRQ(ierr);
		phsar[i]  = ti;                    // PHASE
		ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Par", &ts, 1, 1 ); CHKERRQ(ierr);
		Par[i]    = ts;                    // PARAMETER VALUES
		ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Uba", &ts, 1, 1 ); CHKERRQ(ierr);
		Ubar[i]   = ts;                    // UPPER BOUND
		ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Lba", &ts, 1, 1 ); CHKERRQ(ierr);
		Lbar[i]   = ts;                    // LOWER BOUND
		ierr = getStringParam(fb, _OPTIONAL_, "Inv_Typ", str, NULL); CHKERRQ(ierr);
		if     (!strcmp(str, "rho0"))       { ti = _RHO0_; }
		else if(!strcmp(str, "rhon"))       { ti = _RHON_; }
		else if(!strcmp(str, "rhoc"))       { ti = _RHOC_; }
		else if(!strcmp(str, "eta"))        { ti = _ETA_;  }
		else if(!strcmp(str, "eta0"))       { ti = _ETA0_; }
		else if(!strcmp(str, "n"))          { ti = _N_;    }
		else if(!strcmp(str, "En"))         { ti = _EN_;   }
		else{ SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER, "WARNING: inversion parameter type is not yet implemented \n"); }
		typar[i]     = ti;
		gradar[i]    = 0;                     // GRADIENTS

		fb->blockID++;
	}

	IOparam->phs = phsar;
	IOparam->typ = typar;
	IOparam->grd = gradar;
	VecRestoreArray(P,&Par);
	VecRestoreArray(Ub,&Ubar);
	VecRestoreArray(Lb,&Lbar);
	VecRestoreArray(grad,&gradar);
	IOparam->mdN = i;

	ierr = FBFreeBlocks(fb); CHKERRQ(ierr);

	// LOCATIONS
	// Get location / value / etc.
	ierr = FBFindBlocks(fb, _OPTIONAL_, "<InverseIndStart>", "<InverseIndEnd>"); CHKERRQ(ierr);

	// error checking
	if(fb->nblocks > _MAX_IND_)
	{
		SETERRQ1(PETSC_COMM_WORLD, PETSC_ERR_USER, "Too many inverse indices specified! Max allowed: %lld", (LLD)_MAX_IND_);
	}
	if(!fb->nblocks && IOparam->Ap == 1)
	{
		SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER, "You have to define indices (mdi) for the inversion. Have a look into the comments in src/LaMEM.cpp");
	}

	// Catch that if we have scaling none the index values dont go to inf:
	if(scal.utype == _NONE_)
	{
		scal.length   = 1;
		scal.velocity = 1;
	}

	// read each individual index
	for(i = 0; i < fb->nblocks; i++)
	{
		ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Coord",  IOparam->Coord, 3, 1);        CHKERRQ(ierr);
		Ax[i] = (IOparam->Coord[0])/scal.length;
		Ay[i] = (IOparam->Coord[1])/scal.length;
		Az[i] = (IOparam->Coord[2])/scal.length;
		ierr = getIntParam   (fb, _OPTIONAL_, "Inv_Av", &ti, 1, 3); CHKERRQ(ierr);
		Av[i] = ti;                     // VELOCITY COMPONENT
		ierr = getScalarParam(fb, _OPTIONAL_, "Inv_Ae", &ts, 1, 1 ); CHKERRQ(ierr);
		Ae[i] = ts /scal.velocity;     // VELOCITY VALUE
		fb->blockID++;
	}

	IOparam->Ax = Ax;
	IOparam->Ay = Ay;
	IOparam->Az = Az;
	IOparam->Av = Av;
	IOparam->Ae = Ae;
	IOparam->mdI = i;

	ierr = FBFreeBlocks(fb); CHKERRQ(ierr);

	// Error checking
	if (IOparam->use != 0 && !IOparam->Ap)
	{
		SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER, "You have to define indices (Ap) for the inversion. Have a look into the comments in src/LaMEM.cpp");
	}

	//===============
	// SOLVE ADJOINT (LaMIMLibMain)
	//===============
	// only compute the adjoint gradients or simply forward code
	if(IOparam->use == 2)
 	{
 		VecDuplicate(P,&IOparam->P);
 		VecCopy(P,IOparam->P);

 		// call LaMEM main library function
 		ierr = LaMEMLibMain(IOparam); CHKERRQ(ierr);
 	}
 	// compute 'full' adjoint inversion
 	else if(IOparam->use == 3)
 	{
 		VecDuplicate(P,&IOparam->P);
 		VecCopy(P,IOparam->P);

 		// if tao is used try the LMVM/BLMVM algorithms
 		if(IOparam->Tao == 1)
 		{
 	 		Tao tao;
			TaoLineSearch ls;

 	 		ierr = TaoCreate(PETSC_COMM_WORLD,&tao); CHKERRQ(ierr);

 	 	 	// 1.Check if bounds are available and sets them
 	 		if (IOparam->Ab == 1)
 	 		{
 	 	 	 	ierr = TaoSetVariableBounds(tao,Lb,Ub);	 								CHKERRQ(ierr);
 	 	 	 	ierr = TaoSetType(tao,TAOBLMVM);CHKERRQ(ierr);
 	 		}
 	 		else
 	 		{
 	 			ierr = TaoSetType(tao,TAOLMVM);CHKERRQ(ierr);                    // TAOLMVM, TAOBLMVM, TAOBMRM (bad), TAOCG (all 4), TAOTRON (might crash but is fast)
 	 		}

 	 		// 2. Set up Tao
 	 	 	ierr = TaoSetObjectiveAndGradientRoutine(tao, AdjointOptimisationTAO, IOparam);	 	CHKERRQ(ierr);
 	 	 	ierr = TaoSetInitialVector(tao,P);	 									CHKERRQ(ierr);
 	 	 	ierr = TaoSetTolerances(tao,1e-30,1e-30,1e-30);	CHKERRQ(ierr);
 	 	 	ierr = TaoSetFunctionLowerBound(tao,1e-5);CHKERRQ(ierr);
 	 	 	ierr = TaoSetFromOptions(tao);	 										CHKERRQ(ierr);
			
			// Line-Search
			ierr = TaoGetLineSearch(tao, &ls);   CHKERRQ(ierr);
			ierr = TaoLineSearchSetFromOptions(ls);   CHKERRQ(ierr);

 	 	 	// 3. Solve Tao & view result
 	 	 	ierr = TaoSolve(tao);	 												CHKERRQ(ierr);
 	 	 	PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n");
 	 	 	TaoView(tao,PETSC_VIEWER_STDOUT_WORLD);
 	 	 	PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n");

 	 	 	// 4. Clean
 	 	 	ierr = TaoDestroy(&tao);
 		}
 		else
 		// Without TAO try line search tuned gradient descent
 		{
 			ierr = AdjointOptimisation(P, F, grad, IOparam);
 		}

 		PetscPrintf(PETSC_COMM_WORLD,"\n------------------------------------------\n");
 		PetscPrintf(PETSC_COMM_WORLD,"*         INVERSION RESULT SUMMARY       *\n");
 		PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n");
 		PetscPrintf(PETSC_COMM_WORLD,"Number of inversion iterations: %d\n",IOparam->count);
 		PetscPrintf(PETSC_COMM_WORLD,"F/Fini:\n");
 		VecGetArray(IOparam->fcconv,&fcconvar);
 		for(i=1;i<IOparam->count;i++)
 		{
 			PetscPrintf(PETSC_COMM_WORLD,"%.5e\n",fcconvar[i]);
 		}
 		VecRestoreArray(IOparam->fcconv,&fcconvar);
 		PetscPrintf(PETSC_COMM_WORLD,"\nFinal cost function:\n");
 		PetscPrintf(PETSC_COMM_WORLD,"%.5e\n",IOparam->mfit);
 		PetscPrintf(PETSC_COMM_WORLD,"\nFinal Parameters:\n");
		VecGetArray(IOparam->P,&Par);
		for(i=0;i<IOparam->mdN;i++)
		{
			PetscPrintf(PETSC_COMM_WORLD,"%.5e\n",Par[i]);
		}
		VecRestoreArray(IOparam->P,&Par);
 		PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n\n");

 	}
 	// this is a forward simulation that we want to save as comparison solution
 	else if(IOparam->use == 4)
 	{
 		// call LaMEM main library function
 		ierr = LaMEMLibMain(IOparam); CHKERRQ(ierr);

 		// Save output
 		PetscViewer     viewerVel;
 		PetscViewerCreate(PETSC_COMM_WORLD,&viewerVel);
 		PetscViewerSetType(viewerVel,PETSCVIEWERBINARY);
 		PetscViewerFileSetMode(viewerVel,FILE_MODE_WRITE);
 		PetscViewerFileSetName(viewerVel,"Forward_Solution_Vel.bin");
 	 	VecView(IOparam->xini,viewerVel);
 	 	PetscViewerDestroy(&viewerVel);

 	 	PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n        Forward Solution succesfully saved\n------------------------------------------\n");
 	}

	ierr = VecDestroy(&P);
	ierr = VecDestroy(&Ub);
	ierr = VecDestroy(&Lb);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointDestroy"
PetscErrorCode AdjointDestroy(AdjGrad *aop)
{
	PetscErrorCode ierr;
	PetscFunctionBegin;

	// Destroy the Adjoint gradients structures
	ierr = PetscMemzero(aop, sizeof(AdjGrad)); CHKERRQ(ierr);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointOptimisation"
PetscErrorCode AdjointOptimisation(Vec P, PetscScalar F, Vec grad, void *ctx)
{
	PetscErrorCode ierr;
	PetscFunctionBegin;

	// initialize
	PetscInt 		i, j, LScount;
	PetscScalar 	*Par, *Paroldar, *gradar, *dPtemp, *fcconvar;
	PetscScalar   	Fold;
	ModParam    	*IOparam;
	IOparam     	= (ModParam*)ctx;
	Vec         	dP,dgrad,Pold,gradold,r;

	// get parameter values
	VecDuplicate(IOparam->P,&dP);
	VecDuplicate(IOparam->P,&Pold);
	VecDuplicate(grad,&gradold);
	VecDuplicate(grad,&dgrad);
	VecDuplicate(grad,&r);
	VecCopy(P,IOparam->P);

	// Initialize cost functions
	F = 1e100;
	Fold = 1e100;

	while(F>IOparam->tol)
	{
		// Give the updated values to the code
		VecCopy(P,IOparam->P);

		// Reset line search counter
		LScount = 1;

		// call LaMEM main library function
		ierr = LaMEMLibMain(IOparam); CHKERRQ(ierr);

		// Save intial cost function & create initial Hessian
		if(IOparam->count==1)
		{
			IOparam->mfitini = IOparam->mfit;
		}

		// Save cost function
		F = IOparam->mfit;

		// If cost function in this timestep is larger then before perform bisection line search
		while(F>Fold)
		{
			PetscPrintf(PETSC_COMM_WORLD,"\n- - - - - - - - - - - - - - - - - - - - - - - - - - - \n");
			PetscPrintf(PETSC_COMM_WORLD,"              LINE SEARCH IT %d                       \n",LScount);

			VecGetArray(P,&Par);
			VecGetArray(Pold,&Paroldar);
			VecGetArray(dP,&dPtemp);
			for(i=0;i<IOparam->mdN;i++)
			{
				for(j=0;j<IOparam->mdN;j++)
				{
					dPtemp[i] = dPtemp[i]*IOparam->facB;
				}
			}

			// Update parameter
			for(i=0;i<IOparam->mdN;i++)
			{
				Par[i] = Paroldar[i] + dPtemp[i];
			}
			VecRestoreArray(P,&Par);
			VecRestoreArray(Pold,&Paroldar);
			VecRestoreArray(dP,&dPtemp);

			// Give the updated values to the code
			VecCopy(P,IOparam->P);

			// call LaMEM main library function
			ierr = LaMEMLibMain(IOparam); CHKERRQ(ierr);

			F = IOparam->mfit;

			LScount+=1;
			if(LScount>IOparam->maxitLS)
			{
				PetscPrintf(PETSC_COMM_WORLD,"******************************************************\n");
				PetscPrintf(PETSC_COMM_WORLD,"*             YOUR SOLUTION DIVERGED                 *\n");
				PetscPrintf(PETSC_COMM_WORLD,"******************************************************\n\n");

				// Return parameters for final output
				VecCopy(P,IOparam->P);

				PetscFunctionReturn(0);
			}
		}

		// Zero out perturbation
		VecDuplicate(IOparam->P,&dP);

		// restore parameter values
		VecDuplicate(IOparam->P,&P);
		VecCopy(IOparam->P,P);

		VecGetArray(P,&Par);
		VecGetArray(grad,&gradar);
		for(j = 0; j < IOparam->mdN; j++)
		{
			gradar[j] = IOparam->grd[j];
			if (IOparam->count==1)
			{
				IOparam->factor2array[j] = fabs((IOparam->Scale_Grad*Par[j])/gradar[j]);
			}
		}
		VecRestoreArray(grad,&gradar);
		VecRestoreArray(P,&Par);

		PetscPrintf(PETSC_COMM_WORLD,"\n------------------------------------------\n");
		PetscPrintf(PETSC_COMM_WORLD,"%d. IT INVERSION RESULT: line search its = %d ; F / FINI = %.5e\n\n",IOparam->count,LScount-1,IOparam->mfit/IOparam->mfitini);
		PetscPrintf(PETSC_COMM_WORLD,"FOLD = %.5e \n   F = %.5e\n\n",Fold,F);

		// BEFORE UPDATING the par vector store the old gradient & Parameter vector (for BFGS)
		VecCopy(P,Pold);
		VecCopy(grad,gradold);
		Fold = F;

		VecGetArray(grad,&gradar);
		VecGetArray(P,&Par);
		VecGetArray(dP,&dPtemp);
		for(i=0;i<IOparam->mdN;i++)
		{
			dPtemp[i] = - (dPtemp[i] + (gradar[i])) * IOparam->factor2array[i];
			IOparam->factor2array[i] *= IOparam->facLS;
			if(IOparam->factor2array[i]>IOparam->maxfac)
			{
				IOparam->factor2array[i] = IOparam->maxfac;
			}
			PetscPrintf(PETSC_COMM_WORLD,"LS factor for %d.Parameter = %.5e\n",i+1,IOparam->factor2array[i]);
		}	
		PetscPrintf(PETSC_COMM_WORLD,"\n");

		// Display the current state of the parameters
		for(j = 0; j < IOparam->mdN; j++)
		{
			PetscPrintf(PETSC_COMM_WORLD,"%D. Diff parameter value = %.5e\n",j+1,dPtemp[j]);
		}

		PetscPrintf(PETSC_COMM_WORLD,"\n");

		// Update parameter
		for(i=0;i<IOparam->mdN;i++)
		{
			if(F>IOparam->tol)
			{
				Par[i] = Par[i] + dPtemp[i];
			}
		}
		VecRestoreArray(grad,&gradar);
		VecRestoreArray(P,&Par);
		VecRestoreArray(dP,&dPtemp);

		// Display the current state of the parameters
		VecGetArray(P,&Par);
		for(j = 0; j < IOparam->mdN; j++)
		{
			PetscPrintf(PETSC_COMM_WORLD,"%D. Parameter value = %.5e\n",j+1,Par[j]);
		}
		VecRestoreArray(P,&Par);

		PetscPrintf(PETSC_COMM_WORLD,"------------------------------------------\n\n");

		// Give the updated values to the code  (actually unfortunately necessary here and at the top of this function - need to rearrange that)
		VecCopy(P,IOparam->P);

		VecGetArray(IOparam->fcconv,&fcconvar);
		fcconvar[IOparam->count] = IOparam->mfit/IOparam->mfitini;
		VecRestoreArray(IOparam->fcconv,&fcconvar);

		// count
		IOparam->count += 1;
		if(IOparam->count>IOparam->maxit)
		{
			PetscPrintf(PETSC_COMM_WORLD,"\n\nMaximum number of invere iterations reached\n\n");
			break;
		}
	}
	PetscFunctionReturn(0);
}

//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointOptimisationTAO"
PetscErrorCode AdjointOptimisationTAO(Tao tao, Vec P, PetscReal *F, Vec grad, void *ctx)
{
	PetscErrorCode ierr;
	PetscFunctionBegin;

	// initialize
	PetscInt j;
	PetscScalar *Par, *gradar, *fcconvar;
	ModParam    *IOparam;
	IOparam     = (ModParam*)ctx;

	// get parameter values
	VecDuplicate(P,&IOparam->P);
	VecCopy(P,IOparam->P);

	// call LaMEM main library function
	ierr = LaMEMLibMain(IOparam); CHKERRQ(ierr);

	// restore parameter values
	VecDuplicate(IOparam->P,&P);
	VecCopy(IOparam->P,P);

	// Store the gradient & misfit
	VecGetArray(grad,&gradar);
	for(j = 0; j < IOparam->mdN; j++)
	{
		gradar[j] = IOparam->grd[j];
	}
	VecRestoreArray(grad,&gradar);

	*F = IOparam->mfit;

	// Save intial cost function
	if(IOparam->count==1){IOparam->mfitini = IOparam->mfit;}

	// Display the current state of the parameters
	VecGetArray(P,&Par);
	for(j = 0; j < IOparam->mdN; j++)
	{
		PetscPrintf(PETSC_COMM_WORLD,"%D. Parameter value = %.5e\n",j+1,Par[j]);
	}
	VecRestoreArray(P,&Par);

	// Relative cost function
	PetscPrintf(PETSC_COMM_WORLD,"mfit / mfit0 = %.5e\n------------------------------------------\n\n",IOparam->mfit/IOparam->mfitini);

	VecGetArray(IOparam->fcconv,&fcconvar);
	fcconvar[IOparam->count] = IOparam->mfit/IOparam->mfitini;
	VecRestoreArray(IOparam->fcconv,&fcconvar);

	// count
	IOparam->count += 1;
	if(IOparam->count>1500)
	{
		PetscPrintf(PETSC_COMM_WORLD,"\n\n\nEXCEEDED 1500 FUNCTION EVALUATIONS (consider changing inversion options)\n\n\n");
		PetscFunctionReturn(0);
	}

	PetscFunctionReturn(0);
}
 //---------------------------------------------------------------------------
 #undef __FUNCT__
 #define __FUNCT__ "AdjointObjectiveAndGradientFunction"
 PetscErrorCode AdjointObjectiveAndGradientFunction(AdjGrad *aop, JacRes *jr, NLSol *nl, ModParam *IOparam, SNES snes, FreeSurf *surf)
 {

	Scaling             *scal;
	Vec                  xini;

 	PetscErrorCode ierr;
 	PetscFunctionBegin;

 	scal = jr->scal;

	//========================================
	// COMPUTE OBJECTIVE FUNCTION & GRADIENT
	//========================================
 	// only compute the gradients
 	if (IOparam->use == 2)
 	{

 	 	// Create projection vector
 	 	ierr = VecDuplicate(jr->gsol, &aop->pro);	 	CHKERRQ(ierr);
 	 	ierr = VecDuplicate(jr->gsol, &IOparam->xini);  CHKERRQ(ierr);
 	 	ierr = VecDuplicate(jr->gsol, &xini);           CHKERRQ(ierr);
 	 	ierr = VecDuplicate(jr->gsol, &aop->dF);           CHKERRQ(ierr);

 		// Put the proportion into the Projection vector where the user defined the computation coordinates (P) & get the velocities
 		ierr = AdjointPointInPro(jr, aop, IOparam, surf); 	CHKERRQ(ierr);

 		ierr = VecCopy(IOparam->xini,xini);                                     CHKERRQ(ierr);

		if (IOparam->Gr == 1)
		{
			// -------- Only get gradients with respect to the solution --------
 			ierr = VecCopy(aop->pro,aop->dF); 				CHKERRQ(ierr);
		}
		else if(IOparam->Gr == 0)
		{
 			// -------- Get gradients with respect of cost function -------------
			PetscPrintf(PETSC_COMM_WORLD,"******************************************\n      COMPUTATION OF THE COST FUNCTION\n******************************************\n");

			PetscScalar Ad;

			// Incorporate projection vector (F = (1/2)*[P*(x-x_ini)' * P*(x-x_ini)])
			ierr = VecAYPX(xini,-1,jr->gsol);                                       CHKERRQ(ierr);
			ierr = VecPointwiseMult(xini, xini,aop->pro);                           CHKERRQ(ierr);

			// Compute objective function value (F = (1/2)*[P*(x-x_ini)' * P*(x-x_ini)])
			ierr 	           = VecDot(xini,xini,&Ad);
			Ad 		          /= 2;
			IOparam->mfit 	   = Ad*pow(scal->velocity,2); // Dimensional misfit function

	 		// Compute it's derivative (dF/dx = P*x-P*x_ini)
	 		ierr = VecDuplicate(jr->gsol, &aop->dF);        CHKERRQ(ierr);
	 		ierr = VecCopy(xini,aop->dF); 		            CHKERRQ(ierr);

	 		PetscPrintf(PETSC_COMM_WORLD,"Current Cost function = %.10e\n",IOparam->mfit);
		}
		else
		{
			PetscPrintf(PETSC_COMM_WORLD,"ERROR choose Inv_Gr = 0 or = 1\n");
		}
		

 		// Get the gradients
 		ierr = AdjointComputeGradients(jr, aop, nl, snes, IOparam, surf);        CHKERRQ(ierr);

 		// Destroy
 		 ierr = VecDestroy(&xini);
 	}
 	// compute 'full' adjoint inversion
 	else if(IOparam->use == 3)
	{
 		PetscScalar Ad;

 	 	// Create projection vector
 	 	ierr = VecDuplicate(jr->gsol, &aop->pro);                                CHKERRQ(ierr);

 	 	if(IOparam->count == 1)
 	 	{
 	 		// We need to read the comparison solution
 	 	 	PetscErrorCode  ierrp;

 	 	 	ierr = VecDuplicate(jr->gsol, &IOparam->xini);                       CHKERRQ(ierr);

 	 	 	if(IOparam->OFdef != 1)
 	 	 	{
 	 	 		PetscViewer     viewerVel;

 	 	 	 	// Load the comparison solution vector
 	 	 	 	PetscViewerBinaryOpen(PETSC_COMM_WORLD,"Forward_Solution_Vel.bin",FILE_MODE_READ,&viewerVel);
 	 	 	 	ierrp = VecLoad(IOparam->xini,viewerVel);                           CHKERRQ(ierrp);

 	 	 	 	if (ierrp){PetscPrintf(PETSC_COMM_WORLD,"ADJOINT ERROR: Could not load the initial solution (xini)\n");PetscFunctionReturn(1);}

 	 	 	    // Destroy
 	 	 	  	PetscViewerDestroy(&viewerVel);

 	 	 	}
 	 	}

 		// Put the proportion into the Projection vector where the user defined the computation coordinates (P) & get the velocities
 		ierr = AdjointPointInPro(jr, aop, IOparam, surf);                       CHKERRQ(ierr);

 		PetscPrintf(PETSC_COMM_WORLD,"******************************************\n      COMPUTATION OF THE COST FUNCTION\n******************************************\n");

	 	// Copy temporary comparison solution
	 	ierr = VecDuplicate(jr->gsol, &xini);                                   CHKERRQ(ierr);
	 	ierr = VecCopy(IOparam->xini,xini);                                     CHKERRQ(ierr);
		
 		// Incorporate projection vector (F = (1/2)*[P*(x-x_ini)' * P*(x-x_ini)])
 		ierr = VecAYPX(xini,-1,jr->gsol);                                       CHKERRQ(ierr);
 		ierr = VecPointwiseMult(xini, xini,aop->pro);                           CHKERRQ(ierr);
 		// Compute objective function value (F = (1/2)*[P*(x-x_ini)' * P*(x-x_ini)])
 		ierr 	           = VecDot(xini,xini,&Ad);
 		Ad 		          /= 2;
 		IOparam->mfit 	   = Ad*pow(scal->velocity,2); // Dimensional misfit function

		 // Compute it's derivative (dF/dx = P*x-P*x_ini)
		ierr = VecDuplicate(jr->gsol, &aop->dF);        CHKERRQ(ierr);
		ierr = VecCopy(xini,aop->dF); 		            CHKERRQ(ierr);
		
 		PetscPrintf(PETSC_COMM_WORLD,"Current Cost function = %.5e\n",IOparam->mfit);

 		// Get the gradients
 		ierr = AdjointComputeGradients(jr, aop, nl, snes, IOparam, surf);        CHKERRQ(ierr);

 		// Destroy
 		ierr = VecDestroy(&xini);
	}
 	else
 	{
 	 	PetscPrintf(PETSC_COMM_WORLD,"ADJOINT ERROR: ComputeAdjointGradient value is not defined ; Choose between [1-2]\n");
 	 	PetscFunctionReturn(1);
 	}

 	PetscFunctionReturn(0);
 }
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointComputeGradients"
PetscErrorCode AdjointComputeGradients(JacRes *jr, AdjGrad *aop, NLSol *nl, SNES snes, ModParam *IOparam, FreeSurf *surf)
{
	PetscPrintf(PETSC_COMM_WORLD,"******************************************\n      COMPUTATION OF THE GRADIENTS\n******************************************\n");

	PetscErrorCode ierr;
	PetscFunctionBegin;

	FDSTAG              *fs;
	KSP                 ksp;
	KSPConvergedReason  reason;
	PetscInt            i, j, CurPhase, CurPar, lrank, grank, fd;
	PetscScalar         dt, grd, Perturb, coord_local[3], *vx, *vy, *vz;
	Vec 				rpl, sol, psi, drdp, res, Perturb_vec;
	PC                  ipc;
	Scaling             *scal;

	fs = jr->fs;
	dt = jr->ts->dt;
	fd = 0;          // Counts FD approximations

	// Profile time
	PetscLogDouble     cputime_start, cputime_end;
	PetscTime(&cputime_start);

	scal = jr->scal;

	// Create all needed vectors in the same size as the solution vector
	ierr = VecDuplicate(jr->gsol, &psi);	 	 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gres, &rpl);		 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gres, &res);	 	 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gsol, &sol); 		 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gsol, &drdp);	 	 CHKERRQ(ierr);
	ierr = VecCopy(jr->gsol,sol); 				 CHKERRQ(ierr);
	ierr = VecCopy(jr->gres,res); 				 CHKERRQ(ierr);

	//========
	// SOLVE
	//========
	// Solve the adjoint equation (psi = J^-T * dF/dx)
	// (A side note that I figured out, ksp still sometimes results in a > 0 gradient even if cost function is zero.. possibly really bad condition number?)

	ierr = SNESGetKSP(snes, &ksp);         		CHKERRQ(ierr);
	ierr = KSPSetOptionsPrefix(ksp,"as_"); 		CHKERRQ(ierr);
	ierr = KSPSetFromOptions(ksp);         		CHKERRQ(ierr);
	ierr = KSPGetPC(ksp, &ipc);            		CHKERRQ(ierr);
	ierr = PCSetType(ipc, PCMAT);          		CHKERRQ(ierr);
	ierr = KSPSetOperators(ksp,nl->J,nl->P);	CHKERRQ(ierr);
	ierr = KSPSolve(ksp,aop->dF,psi);	CHKERRQ(ierr);
	ierr = KSPGetConvergedReason(ksp,&reason);	CHKERRQ(ierr);

	// Field sensitivity or 'classic' phase based gradients?
	if(IOparam->FS == 1)
	{
		CurPar   = IOparam->typ[0];

		// Compute residual with the converged Jacobian analytically
		if (CurPar==_RHO0_)
		{
			aop->CurScal   = (scal->velocity)/(scal->density);
			aop->CurScalst = 1/(scal->density);
			aop->Perturb   = 1e-6;
			ierr = AdjointFormResidualFieldFDRho(snes, sol, psi, nl, aop);          CHKERRQ(ierr);
		}
		else 
		{
			PetscPrintf(PETSC_COMM_WORLD,"Field based gradient only for density programmed!");
		}
	}
	else
	{
		//=================
		// PARAMETER LOOP
		//=================
		for(j = 0; j < IOparam->mdN; j++)
		{
			// Get residual since it is overwritten in VecAYPX
			ierr = VecDuplicate(jr->gres, &res);  	CHKERRQ(ierr);
			ierr = VecCopy(jr->gres,res); 			CHKERRQ(ierr);

			// Get current phase and parameter
			CurPhase = IOparam->phs[j];
			CurPar   = IOparam->typ[j];

			// Set perturbation paramter for the finite differences
			aop->Perturb = 1e-6;

			// Perturb the current parameter in the current phase
			ierr = AdjointGradientPerturbParameter(nl, CurPar, CurPhase, aop, scal);   CHKERRQ(ierr);

			// get the actual used perturbation parameter which is 1e-6*parameter
			Perturb = aop->Perturb;
			ierr = VecDuplicate(jr->gsol, &Perturb_vec);          CHKERRQ(ierr);
			ierr = VecSet(Perturb_vec,Perturb);                   CHKERRQ(ierr);

			ierr = FormResidual(snes, sol, rpl, nl);              CHKERRQ(ierr);
			ierr = VecAYPX(res,-1,rpl);                           CHKERRQ(ierr);
			ierr = VecPointwiseDivide(drdp,res,Perturb_vec);      CHKERRQ(ierr);

			// Reset perturbed parameter
			ierr = AdjointGradientResetParameter(nl, CurPar, CurPhase, aop);           CHKERRQ(ierr);

			fd += fd;  // Needed to free vectors later

			// Compute the gradient (dF/dp = -psi^T * dr/dp) & Save gradient
			ierr = VecDot(drdp,psi,&grd);                       CHKERRQ(ierr);
			IOparam->grd[j] 	= -grd*aop->CurScal;

			// Print result
			PetscPrintf(PETSC_COMM_WORLD,"%D.Gradient (dimensional) = %.10e ; CurPar = %d ; CurPhase = %d\n",j+1, IOparam->grd[j], CurPar, CurPhase);
		}

		// Destroy overwritten residual vector
		ierr = VecDestroy(&res);
	}

	if(IOparam->mdI<_MAX_IND_ && IOparam->Ap == 1)
	{

		// get the current velocities at comparison point
		ierr = AdjointPointInPro(jr, aop, IOparam, surf);    CHKERRQ(ierr);

		VecGetArray(aop->vx,&vx);
		VecGetArray(aop->vy,&vy);
		VecGetArray(aop->vz,&vz);

		// Print the solution variable at the user defined index (if they are suffently few)
		for (i=0; i<IOparam->mdI; i++)
		{

			coord_local[0] = IOparam->Ax[i];
			coord_local[1] = IOparam->Ay[i];
			coord_local[2] = IOparam->Az[i];

			// get global & local ranks of a marker
			ierr = FDSTAGGetPointRanks(fs, coord_local, &lrank, &grank); CHKERRQ(ierr);

			// If lrank is not 13 the point is not on this processor
			if(lrank == 13)
			{
				if (IOparam->Av[i] == 1)
				{
					PetscPrintf(PETSC_COMM_SELF,"Current Vx = %.5e at Location x = %g , y = %g , z = %g\n",vx[i]*scal->velocity,IOparam->Ax[i]*scal->length,IOparam->Ay[i]*scal->length,IOparam->Az[i]*scal->length);
				}
				else if (IOparam->Av[i] == 2)
				{
					PetscPrintf(PETSC_COMM_SELF,"Current [Vy] (dimensional) = %.5e at Location x = %g , y = %g , z = %g\n",vy[i]*scal->velocity,IOparam->Ax[i]*scal->length,IOparam->Ay[i]*scal->length,IOparam->Az[i]*scal->length);
				}
				else if (IOparam->Av[i] == 3)
				{
					// PetscPrintf(PETSC_COMM_SELF,"Current [Vz] (dimensional) = %.5e\n",vz[i]*scal->velocity);
					PetscPrintf(PETSC_COMM_SELF,"Current [Vz] (dimensional) = %.10e at Location x = %g , y = %g , z = %g\n",vz[i]*scal->velocity,IOparam->Ax[i]*scal->length,IOparam->Ay[i]*scal->length,IOparam->Az[i]*scal->length);
				}

				if (IOparam->Adv == 1)     // advect the point?
				{
					IOparam->Ax[i] += vx[i]*dt;
					IOparam->Ay[i] += vy[i]*dt;
					IOparam->Az[i] += vz[i]*dt;
				}
			}
		}

		VecRestoreArray(aop->vx,&vx);
		VecRestoreArray(aop->vy,&vy);
		VecRestoreArray(aop->vz,&vz);
	}

	// Clean
	ierr = VecDestroy(&psi);
	ierr = VecDestroy(&sol);
	ierr = VecDestroy(&drdp);
	ierr = VecDestroy(&rpl);
	if(fd > 0)
	{
		ierr = VecDestroy(&Perturb_vec);
	}

	PetscTime(&cputime_end);
	PetscPrintf(PETSC_COMM_WORLD,"Computation was succesful & took %g s\n******************************************\n",cputime_end - cputime_start);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointPointInPro"
PetscErrorCode AdjointPointInPro(JacRes *jr, AdjGrad *aop, ModParam *IOparam, FreeSurf *surf)
{
	PetscErrorCode      ierr;
	FDSTAG              *fs;
	Vec                 lproX, lproY, lproZ, gproX, gproY, gproZ, pro, xini, lxiniX, lxiniY, lxiniZ, gxiniX, gxiniY, gxiniZ;
	PetscScalar         coord_local[3], *temppro, ***llproX, ***llproY, ***llproZ, *dggproX, *dggproY, *dggproZ, *tempxini, ***llxiniX, ***llxiniY, ***llxiniZ, *dggxiniX, *dggxiniY, *dggxiniZ;
	PetscScalar         *vx, *vy, *vz;
	PetscScalar         f1,f2,f3,f4,f5,f6,f7,f8;
	PetscInt            j, i, ii, sx, sy, sz, nx, ny, nz, I, J, K, II, JJ, KK, lrank, grank, level;
	PetscScalar         w, z, xb, yb, zb, xe, ye, ze, xc, yc, zc, *iter, *ncx, *ncy, *ncz, *ccx, *ccy, *ccz, ***lvx, ***lvy, ***lvz, ***vgrid, ***topo, ***vsurf;
	Discret1D           *dsz;
	InterpFlags         iflag;

	PetscFunctionBegin;

	fs = jr->fs;

 	ierr = VecDuplicate(jr->gsol, &pro);             CHKERRQ(ierr);
 	ierr = VecDuplicate(jr->gsol, &xini);            CHKERRQ(ierr);

	// Access the local velocities
	ierr = DMDAVecGetArray(fs->DA_X, jr->lvx, &lvx); CHKERRQ(ierr);
	ierr = DMDAVecGetArray(fs->DA_Y, jr->lvy, &lvy); CHKERRQ(ierr);
	ierr = DMDAVecGetArray(fs->DA_Z, jr->lvz, &lvz); CHKERRQ(ierr);

	ierr = DMCreateGlobalVector(fs->DA_X, &gproX);   CHKERRQ(ierr);
	ierr = DMCreateGlobalVector(fs->DA_Y, &gproY);   CHKERRQ(ierr);
	ierr = DMCreateGlobalVector(fs->DA_Z, &gproZ);   CHKERRQ(ierr);

	ierr = DMCreateLocalVector (fs->DA_X, &lproX);   CHKERRQ(ierr);
	ierr = DMCreateLocalVector (fs->DA_Y, &lproY);   CHKERRQ(ierr);
	ierr = DMCreateLocalVector (fs->DA_Z, &lproZ);   CHKERRQ(ierr);

	ierr = DMCreateGlobalVector(fs->DA_X, &gxiniX);   CHKERRQ(ierr);
	ierr = DMCreateGlobalVector(fs->DA_Y, &gxiniY);   CHKERRQ(ierr);
	ierr = DMCreateGlobalVector(fs->DA_Z, &gxiniZ);   CHKERRQ(ierr);

	ierr = DMCreateLocalVector (fs->DA_X, &lxiniX);   CHKERRQ(ierr);
	ierr = DMCreateLocalVector (fs->DA_Y, &lxiniY);   CHKERRQ(ierr);
	ierr = DMCreateLocalVector (fs->DA_Z, &lxiniZ);   CHKERRQ(ierr);

	// Zero out entries
	VecZeroEntries(gproX);
	VecZeroEntries(gproY);
	VecZeroEntries(gproZ);
	VecZeroEntries(lproX);
	VecZeroEntries(lproY);
	VecZeroEntries(lproZ);

	VecZeroEntries(gxiniX);
	VecZeroEntries(gxiniY);
	VecZeroEntries(gxiniZ);
	VecZeroEntries(lxiniX);
	VecZeroEntries(lxiniY);
	VecZeroEntries(lxiniZ);


	// If we want only a few indices we need to interpolate
	if(IOparam->Ap == 1)
	{
		// Create everything
	 	ierr = VecCreateMPI(PETSC_COMM_WORLD, IOparam->mdI, PETSC_DETERMINE, &aop->vx); CHKERRQ(ierr);
	 	ierr = VecCreateMPI(PETSC_COMM_WORLD, IOparam->mdI, PETSC_DETERMINE, &aop->vy); CHKERRQ(ierr);
	 	ierr = VecCreateMPI(PETSC_COMM_WORLD, IOparam->mdI, PETSC_DETERMINE, &aop->vz); CHKERRQ(ierr);

		ierr = VecGetArray(aop->vx,&vx); CHKERRQ(ierr);
		ierr = VecGetArray(aop->vy,&vy); CHKERRQ(ierr);
		ierr = VecGetArray(aop->vz,&vz); CHKERRQ(ierr);

		//=================
		// INDEXING LOOP
		//=================
		for(ii = 0; ii < IOparam->mdI; ii++)
		{
			// Create coordinate vector
			coord_local[0] = IOparam->Ax[ii];
			coord_local[1] = IOparam->Ay[ii];
			coord_local[2] = IOparam->Az[ii];

			// get global & local ranks of a marker
			ierr = FDSTAGGetPointRanks(fs, coord_local, &lrank, &grank); CHKERRQ(ierr);

			// If lrank is not 13 the point is not on this processor
			if(lrank == 13)
			{
				// starting indices & number of cells
				sx = fs->dsx.pstart; nx = fs->dsx.ncels;
				sy = fs->dsy.pstart; ny = fs->dsy.ncels;
				sz = fs->dsz.pstart; nz = fs->dsz.ncels;

				// node & cell coordinates
				ncx = fs->dsx.ncoor; ccx = fs->dsx.ccoor;
				ncy = fs->dsy.ncoor; ccy = fs->dsy.ccoor;
				ncz = fs->dsz.ncoor; ccz = fs->dsz.ccoor;

				// find I, J, K indices by bisection algorithm
				I = FindPointInCellAdjoint(ncx, 0, nx, coord_local[0]);
				J = FindPointInCellAdjoint(ncy, 0, ny, coord_local[1]);
				K = FindPointInCellAdjoint(ncz, 0, nz, coord_local[2]);

				// get coordinates of cell center
				xc = ccx[I];
				yc = ccy[J];
				zc = ccz[K];

				// map marker on the cells of X, Y, Z & center grids
				if(coord_local[0] > xc) { II = I; } else { II = I-1; }
				if(coord_local[1] > yc) { JJ = J; } else { JJ = J-1; }
				if(coord_local[2] > zc) { KK = K; } else { KK = K-1; }

				ierr = DMDAVecGetArray(fs->DA_X, lproX, &llproX);      CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Y, lproY, &llproY);      CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Z, lproZ, &llproZ);      CHKERRQ(ierr);

				if(IOparam->Av[ii] == 1)
				{
					vx[ii] = InterpLin3D(lvx, I,  JJ, KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ncx, ccy, ccz);
					vy[ii] = InterpLin3D(lvy, II, J,  KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ncy, ccz);
					vz[ii] = InterpLin3D(lvz, II, JJ, K,  sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ccy, ncz);

					// get relative coordinates
					xe = (coord_local[0] - ncx[I])/(ncx[I+1] - ncx[I]); xb = 1.0 - xe;
					ye = (coord_local[1] - ccy[JJ])/(ccy[JJ+1] - ccy[JJ]); yb = 1.0 - ye;
					ze = (coord_local[2] - ccz[KK])/(ccz[KK+1] - ccz[KK]); zb = 1.0 - ze;
	
					llproX[sz+KK  ][sy+JJ  ][sx+I  ] = (xb*yb*zb);    
					llproX[sz+KK  ][sy+JJ  ][sx+I+1] = (xe*yb*zb);  
					llproX[sz+KK  ][sy+JJ+1][sx+I  ] = (xb*ye*zb);  
					llproX[sz+KK  ][sy+JJ+1][sx+I+1] = (xe*ye*zb);  
					llproX[sz+KK+1][sy+JJ  ][sx+I  ] = (xb*yb*ze);  
					llproX[sz+KK+1][sy+JJ  ][sx+I+1] = (xe*yb*ze);  
					llproX[sz+KK+1][sy+JJ+1][sx+I  ] = (xb*ye*ze);   
					llproX[sz+KK+1][sy+JJ+1][sx+I+1] = (xe*ye*ze); 

					if(IOparam->OFdef == 1 )
					{
						ierr = DMDAVecGetArray(fs->DA_X, lxiniX, &llxiniX);      CHKERRQ(ierr);

						f1 = vx[ii]/lvx[sz+KK  ][sy+JJ  ][sx+I  ];
						f2 = vx[ii]/lvx[sz+KK  ][sy+JJ  ][sx+I+1];
						f3 = vx[ii]/lvx[sz+KK  ][sy+JJ+1][sx+I  ];
						f4 = vx[ii]/lvx[sz+KK  ][sy+JJ+1][sx+I+1];
						f5 = vx[ii]/lvx[sz+KK+1][sy+JJ  ][sx+I  ];
						f6 = vx[ii]/lvx[sz+KK+1][sy+JJ  ][sx+I+1];
						f7 = vx[ii]/lvx[sz+KK+1][sy+JJ+1][sx+I  ];
						f8 = vx[ii]/lvx[sz+KK+1][sy+JJ+1][sx+I+1];

						llxiniX[sz+KK  ][sy+JJ  ][sx+I  ] = IOparam->Ae[ii]/f1;
						llxiniX[sz+KK  ][sy+JJ  ][sx+I+1] = IOparam->Ae[ii]/f2;
						llxiniX[sz+KK  ][sy+JJ+1][sx+I  ] = IOparam->Ae[ii]/f3;
						llxiniX[sz+KK  ][sy+JJ+1][sx+I+1] = IOparam->Ae[ii]/f4;
						llxiniX[sz+KK+1][sy+JJ  ][sx+I  ] = IOparam->Ae[ii]/f5;
						llxiniX[sz+KK+1][sy+JJ  ][sx+I+1] = IOparam->Ae[ii]/f6;
						llxiniX[sz+KK+1][sy+JJ+1][sx+I  ] = IOparam->Ae[ii]/f7;
						llxiniX[sz+KK+1][sy+JJ+1][sx+I+1] = IOparam->Ae[ii]/f8;

						ierr = DMDAVecRestoreArray(fs->DA_X, lxiniX, &llxiniX);      CHKERRQ(ierr);
					}
				}
				else if(IOparam->Av[ii] == 2)
				{
					vx[ii] = InterpLin3D(lvx, I,  JJ, KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ncx, ccy, ccz);
					vy[ii] = InterpLin3D(lvy, II, J,  KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ncy, ccz);
					vz[ii] = InterpLin3D(lvz, II, JJ, K,  sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ccy, ncz);

					// get relative coordinates
					xe = (coord_local[0] - ccx[II])/(ccx[II+1] - ccx[II]); xb = 1.0 - xe;
					ye = (coord_local[1] - ncy[J])/(ncy[J+1] - ncy[J]); yb = 1.0 - ye;
					ze = (coord_local[2] - ccz[KK])/(ccz[KK+1] - ccz[KK]); zb = 1.0 - ze;
	
					llproY[sz+KK  ][sy+J  ][sx+II  ] = (xb*yb*zb);    
					llproY[sz+KK  ][sy+J  ][sx+II+1] = (xe*yb*zb);    
					llproY[sz+KK  ][sy+J+1][sx+II  ] = (xb*ye*zb);  
					llproY[sz+KK  ][sy+J+1][sx+II+1] = (xe*ye*zb);     
					llproY[sz+KK+1][sy+J  ][sx+II  ] = (xb*yb*ze);     
					llproY[sz+KK+1][sy+J  ][sx+II+1] = (xe*yb*ze);  
					llproY[sz+KK+1][sy+J+1][sx+II  ] = (xb*ye*ze);   
					llproY[sz+KK+1][sy+J+1][sx+II+1] = (xe*ye*ze); 

					if(IOparam->OFdef == 1 )
					{
						ierr = DMDAVecGetArray(fs->DA_Y, lxiniY, &llxiniY);      CHKERRQ(ierr);

						f1 = vy[ii]/lvy[sz+KK  ][sy+J  ][sx+II  ];
						f2 = vy[ii]/lvy[sz+KK  ][sy+J  ][sx+II+1];
						f3 = vy[ii]/lvy[sz+KK  ][sy+J+1][sx+II  ];
						f4 = vy[ii]/lvy[sz+KK  ][sy+J+1][sx+II+1];
						f5 = vy[ii]/lvy[sz+KK+1][sy+J  ][sx+II  ];
						f6 = vy[ii]/lvy[sz+KK+1][sy+J  ][sx+II+1];
						f7 = vy[ii]/lvy[sz+KK+1][sy+J+1][sx+II  ];
						f8 = vy[ii]/lvy[sz+KK+1][sy+J+1][sx+II+1];

						llxiniY[sz+KK  ][sy+J  ][sx+II  ] = IOparam->Ae[ii]/f1;
						llxiniY[sz+KK  ][sy+J  ][sx+II+1] = IOparam->Ae[ii]/f2;
						llxiniY[sz+KK  ][sy+J+1][sx+II  ] = IOparam->Ae[ii]/f3;
						llxiniY[sz+KK  ][sy+J+1][sx+II+1] = IOparam->Ae[ii]/f4;
						llxiniY[sz+KK+1][sy+J  ][sx+II  ] = IOparam->Ae[ii]/f5;
						llxiniY[sz+KK+1][sy+J  ][sx+II+1] = IOparam->Ae[ii]/f6;
						llxiniY[sz+KK+1][sy+J+1][sx+II  ] = IOparam->Ae[ii]/f7;
						llxiniY[sz+KK+1][sy+J+1][sx+II+1] = IOparam->Ae[ii]/f8;

						ierr = DMDAVecRestoreArray(fs->DA_Y, lxiniY, &llxiniY);      CHKERRQ(ierr);
					}
				}
				else if(IOparam->Av[ii] == 3)
				{
					
					vx[ii] = InterpLin3D(lvx, I,  JJ, KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ncx, ccy, ccz);
					vy[ii] = InterpLin3D(lvy, II, J,  KK, sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ncy, ccz);
					vz[ii] = InterpLin3D(lvz, II, JJ, K,  sx, sy, sz, coord_local[0], coord_local[1], coord_local[2], ccx, ccy, ncz);

					// get relative coordinates
					xe = (coord_local[0] - ccx[II])/(ccx[II+1] - ccx[II]); xb = 1.0 - xe;
					ye = (coord_local[1] - ccy[JJ])/(ccy[JJ+1] - ccy[JJ]); yb = 1.0 - ye;
					ze = (coord_local[2] - ncz[K])/(ncz[K+1] - ncz[K]); zb = 1.0 - ze;

					llproZ[sz+K  ][sy+JJ  ][sx+II  ] = (xb*yb*zb);    
					llproZ[sz+K  ][sy+JJ  ][sx+II+1] = (xe*yb*zb);    
					llproZ[sz+K  ][sy+JJ+1][sx+II  ] = (xb*ye*zb);    
					llproZ[sz+K  ][sy+JJ+1][sx+II+1] = (xe*ye*zb);     
					llproZ[sz+K+1][sy+JJ  ][sx+II  ] = (xb*yb*ze);     
					llproZ[sz+K+1][sy+JJ  ][sx+II+1] = (xe*yb*ze);  
					llproZ[sz+K+1][sy+JJ+1][sx+II  ] = (xb*ye*ze);   
					llproZ[sz+K+1][sy+JJ+1][sx+II+1] = (xe*ye*ze); 

					if(IOparam->OFdef == 1 )
					{
						ierr = DMDAVecGetArray(fs->DA_Z, lxiniZ, &llxiniZ);      CHKERRQ(ierr);

						f1 = vz[ii]/lvz[sz+K  ][sy+JJ  ][sx+II  ];
						f2 = vz[ii]/lvz[sz+K  ][sy+JJ  ][sx+II+1];
						f3 = vz[ii]/lvz[sz+K  ][sy+JJ+1][sx+II  ];
						f4 = vz[ii]/lvz[sz+K  ][sy+JJ+1][sx+II+1];
						f5 = vz[ii]/lvz[sz+K+1][sy+JJ  ][sx+II  ];
						f6 = vz[ii]/lvz[sz+K+1][sy+JJ  ][sx+II+1];
						f7 = vz[ii]/lvz[sz+K+1][sy+JJ+1][sx+II  ];
						f8 = vz[ii]/lvz[sz+K+1][sy+JJ+1][sx+II+1];

						llxiniZ[sz+K  ][sy+JJ  ][sx+II  ] = IOparam->Ae[ii]/f1;
						llxiniZ[sz+K  ][sy+JJ  ][sx+II+1] = IOparam->Ae[ii]/f2;
						llxiniZ[sz+K  ][sy+JJ+1][sx+II  ] = IOparam->Ae[ii]/f3;
						llxiniZ[sz+K  ][sy+JJ+1][sx+II+1] = IOparam->Ae[ii]/f4;
						llxiniZ[sz+K+1][sy+JJ  ][sx+II  ] = IOparam->Ae[ii]/f5;
						llxiniZ[sz+K+1][sy+JJ  ][sx+II+1] = IOparam->Ae[ii]/f6;
						llxiniZ[sz+K+1][sy+JJ+1][sx+II  ] = IOparam->Ae[ii]/f7;
						llxiniZ[sz+K+1][sy+JJ+1][sx+II+1] = IOparam->Ae[ii]/f8;

						ierr = DMDAVecRestoreArray(fs->DA_Z, lxiniZ, &llxiniZ);      CHKERRQ(ierr);
					}
				}

				ierr = DMDAVecRestoreArray(fs->DA_X, lproX, &llproX);      CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Y, lproY, &llproY);      CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Z, lproZ, &llproZ);      CHKERRQ(ierr);
			}

		}

		ierr = VecRestoreArray(aop->vx,&vx);        CHKERRQ(ierr);
		ierr = VecRestoreArray(aop->vy,&vy);        CHKERRQ(ierr);
		ierr = VecRestoreArray(aop->vz,&vz);        CHKERRQ(ierr);
	}

	// We want the whole domain as comparison
	else if (IOparam->Ap == 2)
	{
		for(ii = 0; ii < 3; ii++)
		{
			if(IOparam->Av[ii] == 1)
			{
				ierr = VecSet(lproX,1);
			}
			else if(IOparam->Av[ii] == 2)
			{
				ierr = VecSet(lproY,1);
			}
			else if(IOparam->Av[ii] == 3)
			{
				ierr = VecSet(lproZ,1);
			}
		}
	}
	else if (IOparam->Ap == 3)     // take the topography velocity as comparison
	{
		for(ii = 0; ii < 3; ii++)
		{
			if (IOparam->Av[ii] == 1)
			{
				dsz   = &fs->dsz;
				level = dsz->rank;
			
				// create column communicator
				ierr = Discret1DGetColumnComm(dsz); CHKERRQ(ierr);
			
				// set interpolation flags
				iflag.update    = PETSC_FALSE;
				iflag.use_bound = PETSC_TRUE;
			
				ierr = DMDAVecRestoreArray(fs->DA_X, jr->lvx, &lvx);   CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_X, lproX, &llproX);      CHKERRQ(ierr);
				
				// interpolate velocity component from grid faces to corners
				ierr = InterpXFaceCorner(fs, jr->lvx, jr->lbcor, iflag); CHKERRQ(ierr);
			
				// load ghost values
				LOCAL_TO_LOCAL(fs->DA_COR, jr->lbcor)
			
				// access topograpy, grid and surface velocity
				ierr = DMDAVecGetArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
			
				// scan all free surface local points
				ierr = DMDAGetCorners(fs->DA_COR, &sx, &sy, &sz, &nx, &ny, NULL); CHKERRQ(ierr);
			
				START_PLANE_LOOP
				{
					
					// get topography
					z = topo[level][j][i];
			
					// check whether point belongs to domain
					if(z >= dsz->gcrdbeg && z < dsz->gcrdend)
					{
						// find containing cell
						K = FindPointInCellAdjoint(dsz->ncoor, 0, dsz->ncels, z);
			
						// get interpolation weight
						w = (z - dsz->ncoor[K])/(dsz->ncoor[K+1] - dsz->ncoor[K]);
						
						llproX[sz+K][j][i]   = 1.0 - w;
						llproX[sz+K+1][j][i] = w;
					}
				}
				END_PLANE_LOOP
	
				// restore access
				ierr = DMDAVecRestoreArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_X, jr->lvx, &lvx); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_X, lproX, &llproX);            CHKERRQ(ierr);
				
			}
			else if (IOparam->Av[ii] == 2)
			{
				dsz   = &fs->dsz;
				level = dsz->rank;
			
				// create column communicator
				ierr = Discret1DGetColumnComm(dsz); CHKERRQ(ierr);
			
				// set interpolation flags
				iflag.update    = PETSC_FALSE;
				iflag.use_bound = PETSC_TRUE;
			
				ierr = DMDAVecRestoreArray(fs->DA_Y, jr->lvy, &lvy);   CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Y, lproY, &llproY);      CHKERRQ(ierr);
				
				// interpolate velocity component from grid faces to corners
				ierr = InterpYFaceCorner(fs, jr->lvy, jr->lbcor, iflag); CHKERRQ(ierr);
			
				// load ghost values
				LOCAL_TO_LOCAL(fs->DA_COR, jr->lbcor)
			
				// access topograpy, grid and surface velocity
				ierr = DMDAVecGetArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
			
				// scan all free surface local points
				ierr = DMDAGetCorners(fs->DA_COR, &sx, &sy, &sz, &nx, &ny, NULL); CHKERRQ(ierr);
			
				START_PLANE_LOOP
				{
					// get topography
					z = topo[level][j][i];
			
					// check whether point belongs to domain
					if(z >= dsz->gcrdbeg && z < dsz->gcrdend)
					{
						// find containing cell
						K = FindPointInCellAdjoint(dsz->ncoor, 0, dsz->ncels, z);
			
						// get interpolation weight
						w = (z - dsz->ncoor[K])/(dsz->ncoor[K+1] - dsz->ncoor[K]);
						
						llproY[sz+K][j][i]   = 1.0 - w;
						llproY[sz+K+1][j][i] = w;
					}
				}
				END_PLANE_LOOP
	
				// restore access
				ierr = DMDAVecRestoreArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Y, jr->lvy, &lvy); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Y, lproY, &llproY);            CHKERRQ(ierr);
			}
			else if (IOparam->Av[ii] == 3)
			{
				dsz   = &fs->dsz;
				level = dsz->rank;
			
				// create column communicator
				ierr = Discret1DGetColumnComm(dsz); CHKERRQ(ierr);
			
				// set interpolation flags
				iflag.update    = PETSC_FALSE;
				iflag.use_bound = PETSC_TRUE;
				
				ierr = DMDAVecRestoreArray(fs->DA_Z, jr->lvz, &lvz);   CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Z, lproZ, &llproZ);      CHKERRQ(ierr);
				
				// interpolate velocity component from grid faces to corners
				ierr = InterpZFaceCorner(fs, jr->lvz, jr->lbcor, iflag); CHKERRQ(ierr);
			
				// load ghost values
				LOCAL_TO_LOCAL(fs->DA_COR, jr->lbcor)
			
				// access topograpy, grid and surface velocity
				ierr = DMDAVecGetArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
			
				// scan all free surface local points
				ierr = DMDAGetCorners(fs->DA_COR, &sx, &sy, &sz, &nx, &ny, NULL); CHKERRQ(ierr);
				
				
			
				START_PLANE_LOOP
				{
					// get topography
					z = topo[level][j][i];
			
					// check whether point belongs to domain
					if(z >= dsz->gcrdbeg && z < dsz->gcrdend)
					{
						// find containing cell
						K = FindPointInCellAdjoint(dsz->ncoor, 0, dsz->ncels, z);
			
						// get interpolation weight
						w = (z - dsz->ncoor[K])/(dsz->ncoor[K+1] - dsz->ncoor[K]);
						
						llproZ[sz+K][j][i]   = 1.0 - w;
						llproZ[sz+K+1][j][i] = w;
					}
				}
				END_PLANE_LOOP
	
				// restore access
				ierr = DMDAVecRestoreArray(fs->DA_COR,    jr->lbcor,    &vgrid); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->vpatch, &vsurf); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(surf->DA_SURF, surf->ltopo,  &topo);  CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Z, jr->lvz, &lvz); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Z, lproZ, &llproZ);            CHKERRQ(ierr);
			}
		}
		
	}

	LOCAL_TO_GLOBAL(fs->DA_X, lproX, gproX);
	LOCAL_TO_GLOBAL(fs->DA_Y, lproY, gproY);
	LOCAL_TO_GLOBAL(fs->DA_Z, lproZ, gproZ);

	LOCAL_TO_GLOBAL(fs->DA_X, lxiniX, gxiniX);
	LOCAL_TO_GLOBAL(fs->DA_Y, lxiniY, gxiniY);
	LOCAL_TO_GLOBAL(fs->DA_Z, lxiniZ, gxiniZ);

	ierr = VecGetArray(gproX, &dggproX);      CHKERRQ(ierr);
	ierr = VecGetArray(gproY, &dggproY);      CHKERRQ(ierr);
	ierr = VecGetArray(gproZ, &dggproZ);      CHKERRQ(ierr);

	ierr = VecGetArray(gxiniX, &dggxiniX);      CHKERRQ(ierr);
	ierr = VecGetArray(gxiniY, &dggxiniY);      CHKERRQ(ierr);
	ierr = VecGetArray(gxiniZ, &dggxiniZ);      CHKERRQ(ierr);

	// Put the proportion into the Projection vector where the user defined the computation coordinates (P)
	ierr = VecGetArray(pro, &temppro);        CHKERRQ(ierr);
	iter = temppro;

	ierr  = PetscMemcpy(iter, dggproX, (size_t)fs->nXFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nXFace;

	ierr  = PetscMemcpy(iter, dggproY, (size_t)fs->nYFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nYFace;

	ierr  = PetscMemcpy(iter, dggproZ, (size_t)fs->nZFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nZFace;

	// restore & destroy
	ierr = VecRestoreArray(pro,  &temppro);         CHKERRQ(ierr);

	// Put the proportion into the comparison solution vector where the user defined the computation coordinates (P)
	ierr = VecGetArray(xini, &tempxini);        CHKERRQ(ierr);
	iter = tempxini;

	ierr  = PetscMemcpy(iter, dggxiniX, (size_t)fs->nXFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nXFace;

	ierr  = PetscMemcpy(iter, dggxiniY, (size_t)fs->nYFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nYFace;

	ierr  = PetscMemcpy(iter, dggxiniZ, (size_t)fs->nZFace*sizeof(PetscScalar)); CHKERRQ(ierr);
	iter += fs->nZFace;

	// restore & destroy
	ierr = VecRestoreArray(xini, &tempxini);         CHKERRQ(ierr);

	ierr = VecRestoreArray(gproX, &dggproX);       CHKERRQ(ierr);
	ierr = VecRestoreArray(gproY, &dggproY);       CHKERRQ(ierr);
	ierr = VecRestoreArray(gproZ, &dggproZ);       CHKERRQ(ierr);

	ierr = VecRestoreArray(gxiniX, &dggxiniX);       CHKERRQ(ierr);
	ierr = VecRestoreArray(gxiniY, &dggxiniY);       CHKERRQ(ierr);
	ierr = VecRestoreArray(gxiniZ, &dggxiniZ);       CHKERRQ(ierr);

	ierr = VecCopy(pro, aop->pro);

	// If it is 0 we want to keep the initially loaded solution and just ignore copying
	if(IOparam->OFdef == 1)
	{
		ierr = VecCopy(xini,IOparam->xini);       CHKERRQ(ierr);
	}

	ierr = VecDestroy(&lproX);
	ierr = VecDestroy(&lproY);
	ierr = VecDestroy(&lproZ);
	ierr = VecDestroy(&gproX);
	ierr = VecDestroy(&gproY);
	ierr = VecDestroy(&gproZ);
	ierr = VecDestroy(&pro);

	ierr = VecDestroy(&lxiniX);
	ierr = VecDestroy(&lxiniY);
	ierr = VecDestroy(&lxiniZ);
	ierr = VecDestroy(&gxiniX);
	ierr = VecDestroy(&gxiniY);
	ierr = VecDestroy(&gxiniZ);
	ierr = VecDestroy(&xini);

	ierr = DMDAVecRestoreArray(fs->DA_X, jr->lvx, &lvx); CHKERRQ(ierr);
	ierr = DMDAVecRestoreArray(fs->DA_Y, jr->lvy, &lvy); CHKERRQ(ierr);
	ierr = DMDAVecRestoreArray(fs->DA_Z, jr->lvz, &lvz); CHKERRQ(ierr);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointGradientPerturbParameter"
PetscErrorCode AdjointGradientPerturbParameter(NLSol *nl, PetscInt CurPar, PetscInt CurPhase, AdjGrad *aop, Scaling *scal)
{
	PetscScalar         ini, perturb, curscal, curscalst;
	Material_t         *mat;

	PetscFunctionBegin;

	mat = nl->pc->pm->jr->dbm->phases;

	// Get the perturbation value & scaling
	perturb   = aop->Perturb;
	curscal   = aop->CurScal;
	curscalst = aop->CurScalst;

	// Perturb the current parameter in the current phase (more to be included)
	if(CurPar==_RHO0_)			// rho
	{
		ini = mat[CurPhase].rho;
		perturb = ini*perturb;
		mat[CurPhase].rho +=  perturb;
		curscal   = (scal->velocity)/(scal->density);
		curscalst = 1/(scal->density);
	}
	else if (CurPar==_RHON_)	    // rho_n
	{
		ini = mat[CurPhase].rho_n;
		perturb = ini*perturb;
		mat[CurPhase].rho_n +=  perturb;
		curscal   = (scal->velocity)/1;
		curscalst = 1/1;
	}
	else if (CurPar==_RHOC_)	    // rho_c
	{
		ini = mat[CurPhase].rho_c;
		perturb = ini*perturb;
		mat[CurPhase].rho_c +=  perturb;
		curscal = (scal->velocity)*(scal->length_si);
		curscalst = 1/1;
	}
	else if (CurPar==_K_)	    // K
	{
		ini = mat[CurPhase].K;
		perturb = ini*perturb;
		mat[CurPhase].K +=  perturb;
		curscal   = (scal->velocity)/(scal->stress_si);
		curscalst = 1/(scal->stress_si);
	}
	else if (CurPar==_KP_)	    // Kp
	{
		ini = mat[CurPhase].Kp;
		perturb = ini*perturb;
		mat[CurPhase].Kp +=  perturb;
		curscal   = (scal->velocity)/1;
		curscalst = 1/1;
	}
	else if (CurPar==_SHEAR_)	    // G
	{
		ini = mat[CurPhase].G;
		perturb = ini*perturb;
		mat[CurPhase].G +=  perturb;
		curscal   = (scal->velocity)/(scal->stress_si);
		curscalst = 1/(scal->stress_si);
	}
	else if (CurPar==_ETA_)	    // Bd
	{
		// This kind of perturbs the whole NEWTONIAN viscosity, consider perturbing the parameters directly
		ini = mat[CurPhase].Bd;
		PetscScalar BdTemp;
		perturb = perturb*(1.0/(2*ini));
		BdTemp = (1.0/(2*ini)) + perturb;//(perturb*(1.0/(2*ini)));
		mat[CurPhase].Bd =  (1.0/(2*BdTemp));
		curscal   = (scal->velocity)/(scal->viscosity);
		curscalst = 1/(scal->viscosity);
	}
	else if (CurPar==_ED_)	    // Ed
	{
		ini = mat[CurPhase].Ed;
		perturb = ini*perturb;
		mat[CurPhase].Ed +=  perturb;
		curscal   = (scal->velocity)/(1);   // Not sure
		curscalst = 1/1;
	}
	else if (CurPar==_VD_)	// Vd
	{
		ini = mat[CurPhase].Vd;
		perturb = ini*perturb;
		mat[CurPhase].Vd +=  perturb;
		curscal   = (scal->velocity)*(scal->stress_si);
		curscalst = 1*(scal->stress_si);
	}
	else if (CurPar==_ETA0_)	// Bn
	{
		// This kind of perturbs the whole DISLOCATION viscosity, consider perturbing the parameters directly
		ini = mat[CurPhase].Bn;
		PetscScalar ViscTemp;

		// -- Uncomment to compute gradient for ETA0 --
		perturb = perturb* (pow(( mat[CurPhase].Bn * pow(2,mat[CurPhase].n) * pow(aop->DII_ref, mat[CurPhase].n-1) *  pow(scal->stress_si, -mat[CurPhase].n) )/ scal->time_si , -1/mat[CurPhase].n));
		ViscTemp = (pow(( mat[CurPhase].Bn * pow(2,mat[CurPhase].n) * pow(aop->DII_ref, mat[CurPhase].n-1) *  pow(scal->stress_si, -mat[CurPhase].n) )/ scal->time_si , -1/mat[CurPhase].n))  + perturb;
		mat[CurPhase].Bn = pow (2.0*ViscTemp, -mat[CurPhase].n) * pow(aop->DII_ref, 1 - mat[CurPhase].n) * (pow(scal->stress_si, mat[CurPhase].n)*scal->time_si);
		// -- Uncomment to compute gradient for BN --
		// perturb = ini*perturb;
		// mat[CurPhase].Bn +=  perturb;
		curscal   = (scal->velocity)/(scal->viscosity);
		curscalst = 1/(scal->viscosity);
	}
	else if (CurPar== _N_)	// n
	{
		ini = mat[CurPhase].n;
		aop->Ini2 = mat[CurPhase].Bn;
		PetscScalar ViscTemp = pow( (aop->Ini2 * pow(2,mat[CurPhase].n) * pow(aop->DII_ref, mat[CurPhase].n-1) * pow(scal->stress_si, -mat[CurPhase].n) ) / scal->time_si, -1/mat[CurPhase].n);
		perturb = ini*perturb;
		mat[CurPhase].n +=  perturb;
		// We also accordingly need to perturb the inverse viscosity in this case
		mat[CurPhase].Bn = (pow(2.0*ViscTemp, -mat[CurPhase].n) * pow(aop->DII_ref, 1 - mat[CurPhase].n)) * (pow(scal->stress_si, mat[CurPhase].n)*scal->time_si);
		curscal   = (scal->velocity)/(1);
		curscalst = 1/1;
	}
	else if (CurPar==_EN_)	// En
	{
		ini = mat[CurPhase].En;
		perturb = ini*perturb;
		mat[CurPhase].En +=  perturb;
		curscal   = (scal->velocity)/(1);    // Not sure
		curscalst = 1/1;
	}
	else if (CurPar==_VN_)	// Vn
	{
		ini = mat[CurPhase].Vn;
		perturb = ini*perturb;
		mat[CurPhase].Vn +=  perturb;
		curscal   = (scal->velocity)*(scal->stress_si);
		curscalst = 1*(scal->stress_si);
	}
	else if (CurPar==_TAUP_)	// taup
	{
		ini = mat[CurPhase].taup;
		perturb = ini*perturb;
		mat[CurPhase].taup +=  perturb;
		curscal   = (scal->velocity)/(scal->stress_si);
		curscalst = 1/(scal->stress_si);
	}
	else if (CurPar==_GAMMA_)	// gamma
	{
		ini = mat[CurPhase].gamma;
		perturb = ini*perturb;
		mat[CurPhase].gamma +=  perturb;
		curscal   = (scal->velocity)/(1);
		curscalst = 1/1;
	}
	else if (CurPar==_Q_)	// q
	{
		ini = mat[CurPhase].q;
		perturb = ini*perturb;
		mat[CurPhase].q +=  perturb;
		curscal   = (scal->velocity)/(1);
		curscalst = 1/1;
	}
	else if (CurPar==_FRICTION_)	// fr
	{
		ini = mat[CurPhase].fr;
		perturb = ini*perturb;
		mat[CurPhase].fr +=  perturb;
		curscal   = (scal->velocity)/scal->angle;
		curscalst = 1/scal->angle;
	}
	else if (CurPar==_COHESION_)	// ch
	{
		ini = mat[CurPhase].ch;
		perturb = ini*perturb;
		mat[CurPhase].ch +=  perturb;
		curscal   = (scal->velocity)/scal->stress_si;
		curscalst = 1/scal->stress_si;
	}
	else if (CurPar==_CP_)	// Cp
	{
		ini = mat[CurPhase].Cp;
		perturb = ini*perturb;
		mat[CurPhase].Cp +=  perturb;
		curscal   = (scal->velocity)/scal->cpecific_heat;
		curscalst = 1/scal->cpecific_heat;
	}
	else if (CurPar==_A_)	// A
	{
		ini = mat[CurPhase].A;
		perturb = ini*perturb;
		mat[CurPhase].A +=  perturb;
		curscal   = (scal->velocity)/scal->heat_production;
		curscalst = 1/scal->heat_production;
	}
	else
	{
		PetscPrintf(PETSC_COMM_WORLD,"ADJOINT ERROR: Definition of the current parameter is not defined ; Choose between [0-15]\n");
		PetscFunctionReturn(1);
	}

	// Store initial value of current parameter & scaling
	aop->Ini 		= ini;
	aop->CurScal 	= curscal;
	aop->CurScalst  = curscalst;
	aop->Perturb    = perturb;

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointGradientResetParameter"
PetscErrorCode AdjointGradientResetParameter(NLSol *nl, PetscInt CurPar, PetscInt CurPhase, AdjGrad *aop)
{
	PetscScalar  ini;
	Material_t  *phases;

	PetscFunctionBegin;

	// Get initial value of currently perturbed parameter
	ini    = aop->Ini;
	phases = nl->pc->pm->jr->dbm->phases;

	// Set the current parameter back to its original value
	if (CurPar==_RHO0_)        		{phases[CurPhase].rho    = ini;
	}else if (CurPar==_RHON_)  		{phases[CurPhase].rho_n  = ini;
	}else if (CurPar==_RHOC_)  		{phases[CurPhase].rho_c  = ini;
	}else if (CurPar==_K_)  		{phases[CurPhase].K      = ini;
	}else if (CurPar==_KP_)  		{phases[CurPhase].Kp     = ini;
	}else if (CurPar==_SHEAR_)  	{phases[CurPhase].G      = ini;
	}else if (CurPar==_ETA_)  		{phases[CurPhase].Bd     = ini;
	}else if (CurPar==_ED_)  		{phases[CurPhase].Ed     = ini;
	}else if (CurPar==_VD_) 		{phases[CurPhase].Vd     = ini;
	}else if (CurPar==_ETA0_) 		{phases[CurPhase].Bn     = ini;
	}else if (CurPar==_N_) 			{phases[CurPhase].n      = ini;   phases[CurPhase].Bn = aop->Ini2;
	}else if (CurPar==_EN_) 		{phases[CurPhase].En     = ini;
	}else if (CurPar==_VN_) 		{phases[CurPhase].Vn     = ini;
	}else if (CurPar==_TAUP_) 		{phases[CurPhase].taup   = ini;
	}else if (CurPar==_GAMMA_) 		{phases[CurPhase].gamma  = ini;
	}else if (CurPar==_Q_) 			{phases[CurPhase].q      = ini;
	}else if (CurPar==_FRICTION_) 	{phases[CurPhase].fr 	 = ini;
	}else if (CurPar==_COHESION_) 	{phases[CurPhase].ch 	 = ini;
	}else if (CurPar==_CP_) 	    {phases[CurPhase].Cp     = ini;
	}else if (CurPar==_A_) 			{phases[CurPhase].A      = ini;}
	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "AdjointFormResidualFieldFDRho"
PetscErrorCode AdjointFormResidualFieldFDRho(SNES snes, Vec x, Vec psi, NLSol *nl, AdjGrad *aop  )
{

	// ONLY PROGRAMMED FOR DENSITY!!
	// -> Just a debug function for the field gradient ...
	// -> This thing produces the negative of the gradient (multiply with minus; or compare abs value)

	ConstEqCtx  ctx;
	JacRes     *jr;
	FDSTAG     *fs;
	BCCtx      *bc;
	SolVarCell *svCell;
	SolVarEdge *svEdge;
	PetscInt    iter, lrank;
	PetscInt    I1, I2, J1, J2, K1, K2;
	PetscInt    i, j, k, nx, ny, nz, sx, sy, sz, mx, my, mz, mcx, mcy, mcz;
	PetscScalar XX, XX1, XX2, XX3, XX4;
	PetscScalar YY, YY1, YY2, YY3, YY4;
	PetscScalar ZZ, ZZ1, ZZ2, ZZ3, ZZ4;
	PetscScalar XY, XY1, XY2, XY3, XY4;
	PetscScalar XZ, XZ1, XZ2, XZ3, XZ4;
	PetscScalar YZ, YZ1, YZ2, YZ3, YZ4;
	PetscScalar bdx, fdx, bdy, fdy, bdz, fdz, dx, dy, dz, Le;
	PetscScalar gx, gy, gz, tx, ty, tz, sxx, syy, szz, sxy, sxz, syz, gres;
	PetscScalar J2Inv, DII, z, rho, Tc, pc, pc_lith, pc_pore, dt, fssa, *grav;
	PetscScalar grdt;
	PetscScalar ***fx,  ***fy,  ***fz, ***vx,  ***vy,  ***vz, ***gc, ***bcp, ***llgradfield;
	PetscScalar ***dxx, ***dyy, ***dzz, ***dxy, ***dxz, ***dyz, ***p, ***T, ***p_lith, ***p_pore;
	Vec         drdp, rpl, Perturb_vec, res;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	// access context
	jr = nl->pc->pm->jr;

	// Create stuff
	ierr = VecDuplicate(jr->gsol, &drdp);	 	 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gres, &res);	 	 CHKERRQ(ierr);
	ierr = VecZeroEntries(jr->lgradfield);	 	 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gres, &rpl);		 CHKERRQ(ierr);
	ierr = VecDuplicate(jr->gsol, &Perturb_vec); CHKERRQ(ierr);
	ierr = VecSet(Perturb_vec,aop->Perturb);     CHKERRQ(ierr);

	/*// apply pressure limit at the first visco-plastic timestep and iteration
    if(jr->ts->istep == 1 && jr->ctrl->pLimPlast == PETSC_TRUE)
    {
    	jr->matLim.presLimFlg = PETSC_TRUE;
	}*/

	// access context
	fs  = jr->fs;
	bc  = jr->bc;

	mcx = fs->dsx.tcels - 1;
	mcy = fs->dsy.tcels - 1;
	mcz = fs->dsz.tcels - 1;

	mx  = fs->dsx.tnods - 1;
	my  = fs->dsy.tnods - 1;
	mz  = fs->dsz.tnods - 1;

	// access residual context variables
	fssa   =  jr->ctrl.FSSA; // density gradient penalty parameter
	grav   =  jr->ctrl.grav; // gravity acceleration
	dt     =  jr->ts->dt;    // time step

	// Recompute correct strainrates (necessary!!)
	// ierr =  JacResGetEffStrainRate(jr);

	// access work vectors
	ierr = DMDAVecGetArray(fs->DA_CEN, jr->lgradfield,&llgradfield);      CHKERRQ(ierr);

	for(PetscInt kk=0;kk<fs->dsx.tcels;kk++)
	{
		for(PetscInt jk=0;jk<fs->dsy.tcels;jk++)
		{
			for(PetscInt ik=0;ik<fs->dsz.tcels;ik++)
			{
				// setup constitutive equation evaluation context parameters
				ierr = setUpConstEq(&ctx, jr); CHKERRQ(ierr);

				// clear local residual vectors
				ierr = VecZeroEntries(jr->lfx); CHKERRQ(ierr);
				ierr = VecZeroEntries(jr->lfy); CHKERRQ(ierr);
				ierr = VecZeroEntries(jr->lfz); CHKERRQ(ierr);
				ierr = VecZeroEntries(jr->gc);  CHKERRQ(ierr);

				// access work vectors
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->gc,      &gc);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->lp,      &p);      CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->lT,      &T);      CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->ldxx,    &dxx);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->ldyy,    &dyy);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->ldzz,    &dzz);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_XY,  jr->ldxy,    &dxy);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_XZ,  jr->ldxz,    &dxz);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_YZ,  jr->ldyz,    &dyz);    CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_X,   jr->lfx,     &fx);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Y,   jr->lfy,     &fy);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Z,   jr->lfz,     &fz);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_X,   jr->lvx,     &vx);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Y,   jr->lvy,     &vy);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_Z,   jr->lvz,     &vz);     CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->lp_lith, &p_lith); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, jr->lp_pore, &p_pore); CHKERRQ(ierr);
				ierr = DMDAVecGetArray(fs->DA_CEN, bc->bcp,     &bcp);    CHKERRQ(ierr);

				//-------------------------------
				// central points
				//-------------------------------
				iter = 0;
				GET_CELL_RANGE(nx, sx, fs->dsx)
				GET_CELL_RANGE(ny, sy, fs->dsy)
				GET_CELL_RANGE(nz, sz, fs->dsz)

				START_STD_LOOP
				{
					// access solution variables
					svCell = &jr->svCell[iter++];

					//=================
					// SECOND INVARIANT
					//=================

					// access strain rates
					XX = dxx[k][j][i];
					YY = dyy[k][j][i];
					ZZ = dzz[k][j][i];

					// x-y plane, i-j indices
					XY1 = dxy[k][j][i];
					XY2 = dxy[k][j+1][i];
					XY3 = dxy[k][j][i+1];
					XY4 = dxy[k][j+1][i+1];

					// x-z plane, i-k indices
					XZ1 = dxz[k][j][i];
					XZ2 = dxz[k+1][j][i];
					XZ3 = dxz[k][j][i+1];
					XZ4 = dxz[k+1][j][i+1];

					// y-z plane, j-k indices
					YZ1 = dyz[k][j][i];
					YZ2 = dyz[k+1][j][i];
					YZ3 = dyz[k][j+1][i];
					YZ4 = dyz[k+1][j+1][i];

					// compute second invariant
					J2Inv = 0.5*(XX*XX + YY*YY + ZZ*ZZ) +
					0.25*(XY1*XY1 + XY2*XY2 + XY3*XY3 + XY4*XY4) +
					0.25*(XZ1*XZ1 + XZ2*XZ2 + XZ3*XZ3 + XZ4*XZ4) +
					0.25*(YZ1*YZ1 + YZ2*YZ2 + YZ3*YZ3 + YZ4*YZ4);

					DII = sqrt(J2Inv);

					//=======================
					// CONSTITUTIVE EQUATIONS
					//=======================

					// access current pressure
					pc = p[k][j][i];

					// current temperature
					Tc = T[k][j][i];

					// access current lithostatic pressure
					pc_lith = p_lith[k][j][i];

					// access current pore pressure (zero if deactivated)
					pc_pore = p_pore[k][j][i];

					// z-coordinate of control volume
					z = COORD_CELL(k, sz, fs->dsz);

					// get characteristic element size
					dx = SIZE_CELL(i, sx, fs->dsx);
					dy = SIZE_CELL(j, sy, fs->dsy);
					dz = SIZE_CELL(k, sz, fs->dsz);
					Le = sqrt(dx*dx + dy*dy + dz*dz);

					// setup control volume parameters
					ierr = setUpCtrlVol(&ctx, svCell->phRat, &svCell->svDev, &svCell->svBulk, pc, pc_lith, pc_pore, Tc, DII, z, Le); CHKERRQ(ierr);

					// evaluate constitutive equations on the cell
					ierr = cellConstEq(&ctx, svCell, XX, YY, ZZ, sxx, syy, szz, gres, rho); CHKERRQ(ierr);

					if ((i)==ik && (j)==jk && (k)==kk)
					{
						// Set perturbation paramter for the finite differences
						// aop->Perturb = hP*rho;
						rho += aop->Perturb;
					}

					// compute gravity terms
					gx = rho*grav[0];
					gy = rho*grav[1];
					gz = rho*grav[2];

					// compute stabilization terms (lumped approximation)
					tx = -fssa*dt*gx;
					ty = -fssa*dt*gy;
					tz = -fssa*dt*gz;

					//=========
					// RESIDUAL
					//=========

					// get mesh steps for the backward and forward derivatives
					bdx = SIZE_NODE(i, sx, fs->dsx);   fdx = SIZE_NODE(i+1, sx, fs->dsx);
					bdy = SIZE_NODE(j, sy, fs->dsy);   fdy = SIZE_NODE(j+1, sy, fs->dsy);
					bdz = SIZE_NODE(k, sz, fs->dsz);   fdz = SIZE_NODE(k+1, sz, fs->dsz);

					// momentum
					fx[k][j][i] -= (sxx + vx[k][j][i]*tx)/bdx + gx/2.0;   fx[k][j][i+1] += (sxx + vx[k][j][i+1]*tx)/fdx - gx/2.0;
					fy[k][j][i] -= (syy + vy[k][j][i]*ty)/bdy + gy/2.0;   fy[k][j+1][i] += (syy + vy[k][j+1][i]*ty)/fdy - gy/2.0;
					fz[k][j][i] -= (szz + vz[k][j][i]*tz)/bdz + gz/2.0;   fz[k+1][j][i] += (szz + vz[k+1][j][i]*tz)/fdz - gz/2.0;

					// pressure boundary constraints
					if(i == 0   && bcp[k][j][i-1] != DBL_MAX) fx[k][j][i]   += -p[k][j][i-1]/bdx;
					if(i == mcx && bcp[k][j][i+1] != DBL_MAX) fx[k][j][i+1] -= -p[k][j][i+1]/fdx;
					if(j == 0   && bcp[k][j-1][i] != DBL_MAX) fy[k][j][i]   += -p[k][j-1][i]/bdy;
					if(j == mcy && bcp[k][j+1][i] != DBL_MAX) fy[k][j+1][i] -= -p[k][j+1][i]/fdy;
					if(k == 0   && bcp[k-1][j][i] != DBL_MAX) fz[k][j][i]   += -p[k-1][j][i]/bdz;
					if(k == mcz && bcp[k+1][j][i] != DBL_MAX) fz[k+1][j][i] -= -p[k+1][j][i]/fdz;

					// mass (volume)
					gc[k][j][i] = gres;
					
				}
				END_STD_LOOP

				//-------------------------------
				// xy edge points
				//-------------------------------
				iter = 0;
				GET_NODE_RANGE(nx, sx, fs->dsx)
				GET_NODE_RANGE(ny, sy, fs->dsy)
				GET_CELL_RANGE(nz, sz, fs->dsz)

				START_STD_LOOP
				{
					// access solution variables
					svEdge = &jr->svXYEdge[iter++];

					//=================
					// SECOND INVARIANT
					//=================

					// check index bounds
					I1 = i;   if(I1 == mx) I1--;
					I2 = i-1; if(I2 == -1) I2++;
					J1 = j;   if(J1 == my) J1--;
					J2 = j-1; if(J2 == -1) J2++;

					// access strain rates
					XY = dxy[k][j][i];

					// x-y plane, i-j indices (i & j - bounded)
					XX1 = dxx[k][J1][I1];
					XX2 = dxx[k][J1][I2];
					XX3 = dxx[k][J2][I1];
					XX4 = dxx[k][J2][I2];

					// x-y plane, i-j indices (i & j - bounded)
					YY1 = dyy[k][J1][I1];
					YY2 = dyy[k][J1][I2];
					YY3 = dyy[k][J2][I1];
					YY4 = dyy[k][J2][I2];

					// x-y plane, i-j indices (i & j - bounded)
					ZZ1 = dzz[k][J1][I1];
					ZZ2 = dzz[k][J1][I2];
					ZZ3 = dzz[k][J2][I1];
					ZZ4 = dzz[k][J2][I2];

					// y-z plane j-k indices (j - bounded)
					XZ1 = dxz[k][J1][i];
					XZ2 = dxz[k+1][J1][i];
					XZ3 = dxz[k][J2][i];
					XZ4 = dxz[k+1][J2][i];

					// x-z plane i-k indices (i - bounded)
					YZ1 = dyz[k][j][I1];
					YZ2 = dyz[k+1][j][I1];
					YZ3 = dyz[k][j][I2];
					YZ4 = dyz[k+1][j][I2];

					// compute second invariant
					J2Inv = XY*XY +
					0.125*(XX1*XX1 + XX2*XX2 + XX3*XX3 + XX4*XX4) +
					0.125*(YY1*YY1 + YY2*YY2 + YY3*YY3 + YY4*YY4) +
					0.125*(ZZ1*ZZ1 + ZZ2*ZZ2 + ZZ3*ZZ3 + ZZ4*ZZ4) +
					0.25 *(XZ1*XZ1 + XZ2*XZ2 + XZ3*XZ3 + XZ4*XZ4) +
					0.25 *(YZ1*YZ1 + YZ2*YZ2 + YZ3*YZ3 + YZ4*YZ4);

					DII = sqrt(J2Inv);

					//=======================
					// CONSTITUTIVE EQUATIONS
					//=======================

					// access current pressure (x-y plane, i-j indices)
					pc = 0.25*(p[k][j][i] + p[k][j][i-1] + p[k][j-1][i] + p[k][j-1][i-1]);

					// current temperature (x-y plane, i-j indices)
					Tc = 0.25*(T[k][j][i] + T[k][j][i-1] + T[k][j-1][i] + T[k][j-1][i-1]);

					// access current lithostatic pressure (x-y plane, i-j indices)
					pc_lith = 0.25*(p_lith[k][j][i] + p_lith[k][j][i-1] + p_lith[k][j-1][i] + p_lith[k][j-1][i-1]);

					// access current pore pressure (x-y plane, i-j indices)
					pc_pore = 0.25*(p_pore[k][j][i] + p_pore[k][j][i-1] + p_pore[k][j-1][i] + p_pore[k][j-1][i-1]);

					// get characteristic element size
					dx = SIZE_NODE(i, sx, fs->dsx);
					dy = SIZE_NODE(j, sy, fs->dsy);
					dz = SIZE_CELL(k, sz, fs->dsz);
					Le = sqrt(dx*dx + dy*dy + dz*dz);

					// setup control volume parameters
					ierr = setUpCtrlVol(&ctx, svEdge->phRat, &svEdge->svDev, NULL, pc, pc_lith, pc_pore, Tc, DII, DBL_MAX, Le); CHKERRQ(ierr);


					// evaluate constitutive equations on the edge
					ierr = edgeConstEq(&ctx, svEdge, XY, sxy); CHKERRQ(ierr);

					//=========
					// RESIDUAL
					//=========

					// get mesh steps for the backward and forward derivatives
					bdx = SIZE_CELL(i-1, sx, fs->dsx);   fdx = SIZE_CELL(i, sx, fs->dsx);
					bdy = SIZE_CELL(j-1, sy, fs->dsy);   fdy = SIZE_CELL(j, sy, fs->dsy);

					// momentum
					fx[k][j-1][i] -= sxy/bdy;   fx[k][j][i] += sxy/fdy;
					fy[k][j][i-1] -= sxy/bdx;   fy[k][j][i] += sxy/fdx;

				}
				END_STD_LOOP

				//-------------------------------
				// xz edge points
				//-------------------------------
				iter = 0;
				GET_NODE_RANGE(nx, sx, fs->dsx)
				GET_CELL_RANGE(ny, sy, fs->dsy)
				GET_NODE_RANGE(nz, sz, fs->dsz)

				START_STD_LOOP
				{
					// access solution variables
					svEdge = &jr->svXZEdge[iter++];

					//=================
					// SECOND INVARIANT
					//=================

					// check index bounds
					I1 = i;   if(I1 == mx) I1--;
					I2 = i-1; if(I2 == -1) I2++;
					K1 = k;   if(K1 == mz) K1--;
					K2 = k-1; if(K2 == -1) K2++;

					// access strain rates
					XZ = dxz[k][j][i];

					// x-z plane, i-k indices (i & k - bounded)
					XX1 = dxx[K1][j][I1];
					XX2 = dxx[K1][j][I2];
					XX3 = dxx[K2][j][I1];
					XX4 = dxx[K2][j][I2];

					// x-z plane, i-k indices (i & k - bounded)
					YY1 = dyy[K1][j][I1];
					YY2 = dyy[K1][j][I2];
					YY3 = dyy[K2][j][I1];
					YY4 = dyy[K2][j][I2];

					// x-z plane, i-k indices (i & k - bounded)
					ZZ1 = dzz[K1][j][I1];
					ZZ2 = dzz[K1][j][I2];
					ZZ3 = dzz[K2][j][I1];
					ZZ4 = dzz[K2][j][I2];

					// y-z plane, j-k indices (k - bounded)
					XY1 = dxy[K1][j][i];
					XY2 = dxy[K1][j+1][i];
					XY3 = dxy[K2][j][i];
					XY4 = dxy[K2][j+1][i];

					// xy plane, i-j indices (i - bounded)
					YZ1 = dyz[k][j][I1];
					YZ2 = dyz[k][j+1][I1];
					YZ3 = dyz[k][j][I2];
					YZ4 = dyz[k][j+1][I2];

					// compute second invariant
					J2Inv = XZ*XZ +
					0.125*(XX1*XX1 + XX2*XX2 + XX3*XX3 + XX4*XX4) +
					0.125*(YY1*YY1 + YY2*YY2 + YY3*YY3 + YY4*YY4) +
					0.125*(ZZ1*ZZ1 + ZZ2*ZZ2 + ZZ3*ZZ3 + ZZ4*ZZ4) +
					0.25 *(XY1*XY1 + XY2*XY2 + XY3*XY3 + XY4*XY4) +
					0.25 *(YZ1*YZ1 + YZ2*YZ2 + YZ3*YZ3 + YZ4*YZ4);

					DII = sqrt(J2Inv);

					//=======================
					// CONSTITUTIVE EQUATIONS
					//=======================

					// access current pressure (x-z plane, i-k indices)
					pc = 0.25*(p[k][j][i] + p[k][j][i-1] + p[k-1][j][i] + p[k-1][j][i-1]);

					// current temperature (x-z plane, i-k indices)
					Tc = 0.25*(T[k][j][i] + T[k][j][i-1] + T[k-1][j][i] + T[k-1][j][i-1]);

					// access current lithostatic pressure (x-z plane, i-k indices)
					pc_lith = 0.25*(p_lith[k][j][i] + p_lith[k][j][i-1] + p_lith[k-1][j][i] + p_lith[k-1][j][i-1]);

					// access current pore pressure (x-z plane, i-k indices)
					pc_pore = 0.25*(p_pore[k][j][i] + p_pore[k][j][i-1] + p_pore[k-1][j][i] + p_pore[k-1][j][i-1]);

					// get characteristic element size
					dx = SIZE_NODE(i, sx, fs->dsx);
					dy = SIZE_CELL(j, sy, fs->dsy);
					dz = SIZE_NODE(k, sz, fs->dsz);
					Le = sqrt(dx*dx + dy*dy + dz*dz);

					// setup control volume parameters
					ierr = setUpCtrlVol(&ctx, svEdge->phRat, &svEdge->svDev, NULL, pc, pc_lith, pc_pore, Tc, DII, DBL_MAX, Le); CHKERRQ(ierr);

					// evaluate constitutive equations on the edge
					ierr = edgeConstEq(&ctx, svEdge, XZ, sxz); CHKERRQ(ierr);

					//=========
					// RESIDUAL
					//=========

					// get mesh steps for the backward and forward derivatives
					bdx = SIZE_CELL(i-1, sx, fs->dsx);   fdx = SIZE_CELL(i, sx, fs->dsx);
					bdz = SIZE_CELL(k-1, sz, fs->dsz);   fdz = SIZE_CELL(k, sz, fs->dsz);

					// momentum
					fx[k-1][j][i] -= sxz/bdz;   fx[k][j][i] += sxz/fdz;
					fz[k][j][i-1] -= sxz/bdx;   fz[k][j][i] += sxz/fdx;

				}
				END_STD_LOOP

				//-------------------------------
				// yz edge points
				//-------------------------------
				iter = 0;
				GET_CELL_RANGE(nx, sx, fs->dsx)
				GET_NODE_RANGE(ny, sy, fs->dsy)
				GET_NODE_RANGE(nz, sz, fs->dsz)

				START_STD_LOOP
				{
					// access solution variables
					svEdge = &jr->svYZEdge[iter++];

					//=================
					// SECOND INVARIANT
					//=================

					// check index bounds
					J1 = j;   if(J1 == my) J1--;
					J2 = j-1; if(J2 == -1) J2++;
					K1 = k;   if(K1 == mz) K1--;
					K2 = k-1; if(K2 == -1) K2++;

					// access strain rates
					YZ = dyz[k][j][i];

					// y-z plane, j-k indices (j & k - bounded)
					XX1 = dxx[K1][J1][i];
					XX2 = dxx[K1][J2][i];
					XX3 = dxx[K2][J1][i];
					XX4 = dxx[K2][J2][i];

					// y-z plane, j-k indices (j & k - bounded)
					YY1 = dyy[K1][J1][i];
					YY2 = dyy[K1][J2][i];
					YY3 = dyy[K2][J1][i];
					YY4 = dyy[K2][J2][i];

					// y-z plane, j-k indices (j & k - bounded)
					ZZ1 = dzz[K1][J1][i];
					ZZ2 = dzz[K1][J2][i];
					ZZ3 = dzz[K2][J1][i];
					ZZ4 = dzz[K2][J2][i];

					// x-z plane, i-k indices (k -bounded)
					XY1 = dxy[K1][j][i];
					XY2 = dxy[K1][j][i+1];
					XY3 = dxy[K2][j][i];
					XY4 = dxy[K2][j][i+1];

					// x-y plane, i-j indices (j - bounded)
					XZ1 = dxz[k][J1][i];
					XZ2 = dxz[k][J1][i+1];
					XZ3 = dxz[k][J2][i];
					XZ4 = dxz[k][J2][i+1];

					// compute second invariant
					J2Inv = YZ*YZ +
					0.125*(XX1*XX1 + XX2*XX2 + XX3*XX3 + XX4*XX4) +
					0.125*(YY1*YY1 + YY2*YY2 + YY3*YY3 + YY4*YY4) +
					0.125*(ZZ1*ZZ1 + ZZ2*ZZ2 + ZZ3*ZZ3 + ZZ4*ZZ4) +
					0.25 *(XY1*XY1 + XY2*XY2 + XY3*XY3 + XY4*XY4) +
					0.25 *(XZ1*XZ1 + XZ2*XZ2 + XZ3*XZ3 + XZ4*XZ4);

					DII = sqrt(J2Inv);

					//=======================
					// CONSTITUTIVE EQUATIONS
					//=======================

					// access current pressure (y-z plane, j-k indices)
					pc = 0.25*(p[k][j][i] + p[k][j-1][i] + p[k-1][j][i] + p[k-1][j-1][i]);

					// current temperature (y-z plane, j-k indices)
					Tc = 0.25*(T[k][j][i] + T[k][j-1][i] + T[k-1][j][i] + T[k-1][j-1][i]);

					// access current lithostatic pressure (y-z plane, j-k indices)
					pc_lith = 0.25*(p_lith[k][j][i] + p_lith[k][j-1][i] + p_lith[k-1][j][i] + p_lith[k-1][j-1][i]);

					// access current pore pressure (y-z plane, j-k indices)
					pc_pore = 0.25*(p_pore[k][j][i] + p_pore[k][j-1][i] + p_pore[k-1][j][i] + p_pore[k-1][j-1][i]);

					// get characteristic element size
					dx = SIZE_CELL(i, sx, fs->dsx);
					dy = SIZE_NODE(j, sy, fs->dsy);
					dz = SIZE_NODE(k, sz, fs->dsz);
					Le = sqrt(dx*dx + dy*dy + dz*dz);

					// setup control volume parameters
					ierr = setUpCtrlVol(&ctx, svEdge->phRat, &svEdge->svDev, NULL, pc, pc_lith, pc_pore, Tc, DII, DBL_MAX, Le); CHKERRQ(ierr);

					// evaluate constitutive equations on the edge
					ierr = edgeConstEq(&ctx, svEdge, YZ, syz); CHKERRQ(ierr);

					//=========
					// RESIDUAL
					//=========

					// get mesh steps for the backward and forward derivatives
					bdy = SIZE_CELL(j-1, sy, fs->dsy);   fdy = SIZE_CELL(j, sy, fs->dsy);
					bdz = SIZE_CELL(k-1, sz, fs->dsz);   fdz = SIZE_CELL(k, sz, fs->dsz);

					// update momentum residuals
					fy[k-1][j][i] -= syz/bdz;   fy[k][j][i] += syz/fdz;
					fz[k][j-1][i] -= syz/bdy;   fz[k][j][i] += syz/fdy;

				}
				END_STD_LOOP

				// restore vectors
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->gc,      &gc);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->lp,      &p);      CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->lT,      &T);      CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->ldxx,    &dxx);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->ldyy,    &dyy);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->ldzz,    &dzz);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_XY,  jr->ldxy,    &dxy);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_XZ,  jr->ldxz,    &dxz);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_YZ,  jr->ldyz,    &dyz);    CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_X,   jr->lfx,     &fx);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Y,   jr->lfy,     &fy);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Z,   jr->lfz,     &fz);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_X,   jr->lvx,     &vx);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Y,   jr->lvy,     &vy);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_Z,   jr->lvz,     &vz);     CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->lp_lith, &p_lith); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->lp_pore, &p_pore); CHKERRQ(ierr);
				ierr = DMDAVecRestoreArray(fs->DA_CEN, bc->bcp,     &bcp);    CHKERRQ(ierr);

				// assemble global residuals from local contributions
				LOCAL_TO_GLOBAL(fs->DA_X, jr->lfx, jr->gfx)
				LOCAL_TO_GLOBAL(fs->DA_Y, jr->lfy, jr->gfy)
				LOCAL_TO_GLOBAL(fs->DA_Z, jr->lfz, jr->gfz)

				// check convergence of constitutive equations
				ierr = checkConvConstEq(&ctx); CHKERRQ(ierr);

				// copy residuals to global vector
				ierr = JacResCopyRes(jr, res); CHKERRQ(ierr);

				ierr = FormResidual(snes, x, rpl, nl);              CHKERRQ(ierr);
				ierr = VecAYPX(res,-1,rpl);                           CHKERRQ(ierr);
				ierr = VecPointwiseDivide(drdp,res,Perturb_vec);      CHKERRQ(ierr);

				// Compute the gradient
				ierr = VecDot(drdp,psi,&grdt);    CHKERRQ(ierr);

				GET_CELL_RANGE(nx, sx, fs->dsx)
				GET_CELL_RANGE(ny, sy, fs->dsy)
				GET_CELL_RANGE(nz, sz, fs->dsz)

				if (ik >= sx && ik < sx+nx && jk >= sy && jk < sy+ny && kk >= sz && kk < sz+nz)
				{
					lrank = 13;
				}
				else
				{
					lrank = 100;
				}

				// If lrank is not 13 the point is not on this processor
				if(lrank == 13)
				{
					llgradfield[kk][jk][ik] = -grdt*aop->CurScal;
				}
			}
		}
	}

	// restore vectors
	ierr = DMDAVecRestoreArray(fs->DA_CEN, jr->lgradfield,&llgradfield);CHKERRQ(ierr);

	LOCAL_TO_LOCAL(fs->DA_CEN, jr->lgradfield);

	// deactivate pressure limit after it has been activated
	// jr->matLim.presLimFlg = PETSC_FALSE;

	ierr = VecDestroy(&drdp);   CHKERRQ(ierr);
	ierr = VecDestroy(&rpl);   CHKERRQ(ierr);
	ierr = VecDestroy(&res);   CHKERRQ(ierr);
	ierr = VecDestroy(&Perturb_vec);   CHKERRQ(ierr);

	PetscFunctionReturn(0);

}