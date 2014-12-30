//---------------------------------------------------------------------------
//........................   MARKER ROUTINES   ..............................
//---------------------------------------------------------------------------
#include "LaMEM.h"
#include "Utils.h"
#include "fdstag.h"
#include "solVar.h"
#include "scaling.h"
#include "tssolve.h"
#include "bc.h"
#include "JacRes.h"
#include "advect.h"
#include "marker.h"
#include "break.h"

/*
#START_DOC#
#END_DOC#
*/
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInit"
PetscErrorCode ADVMarkInit(AdvCtx *actx, UserContext *user)
{
	FDSTAG   *fs;
	PetscInt  nmarkx, nmarky, nmarkz, nummark;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	fs = actx->fs;

	PetscPrintf(PETSC_COMM_WORLD," Starting marker initialization routine\n");

	// restart of simulation - markers read from file
	if (user->restart==1) user->msetup = RESTART;

	// allocate storage for uniform distribution
	if(user->msetup != PARALLEL)
	{
		// get local number of markers
		nmarkx  = fs->dsx.ncels * user->NumPartX;
		nmarky  = fs->dsy.ncels * user->NumPartY;
		nmarkz  = fs->dsz.ncels * user->NumPartZ;
		nummark = nmarkx*nmarky*nmarkz;

		// allocate storage
		ierr = ADVReAllocStorage(actx, nummark); CHKERRQ(ierr);

		// set number of markers
		actx->nummark = nummark;
	}

	// initialize coordinates and add random noise if required for hard-coded setups
	if(user->msetup != PARALLEL
	&& user->msetup != REDUNDANT
	&& user->msetup != RESTART)
	{
		ierr = ADVMarkInitCoord(actx, user); CHKERRQ(ierr);
	}
    
    // display info on-screen
    PetscPrintf(PETSC_COMM_WORLD," Marker setup umployed [msetup] : ");

    
	// initialize marker phase, temperature, etc.
    if     (user->msetup == PARALLEL)   { PetscPrintf(PETSC_COMM_WORLD,"%s\n","parallel");        ierr = ADVMarkInitFileParallel (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == REDUNDANT)  { PetscPrintf(PETSC_COMM_WORLD,"%s\n","redundant");       ierr = ADVMarkInitFileRedundant(actx, user); CHKERRQ(ierr); }
	else if(user->msetup == DIAPIR)     { PetscPrintf(PETSC_COMM_WORLD,"%s\n","diapir");          ierr = ADVMarkInitDiapir       (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == BLOCK)      { PetscPrintf(PETSC_COMM_WORLD,"%s\n","block");           ierr = ADVMarkInitBlock        (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == SUBDUCTION) { PetscPrintf(PETSC_COMM_WORLD,"%s\n","subduction");      ierr = ADVMarkInitSubduction   (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == FOLDING)    { PetscPrintf(PETSC_COMM_WORLD,"%s\n","folding");         ierr = ADVMarkInitFolding      (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == DETACHMENT) { PetscPrintf(PETSC_COMM_WORLD,"%s\n","detachment");      ierr = ADVMarkInitDetachment   (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == SLAB)       { PetscPrintf(PETSC_COMM_WORLD,"%s\n","slab");            ierr = ADVMarkInitSlab         (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == SPHERES)    { PetscPrintf(PETSC_COMM_WORLD,"%s\n","spheres");         ierr = ADVMarkInitSpheres      (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == BANDS)      { PetscPrintf(PETSC_COMM_WORLD,"%s\n","bands");           ierr = ADVMarkInitBands        (actx, user); CHKERRQ(ierr); }
	else if(user->msetup == RESTART)    { PetscPrintf(PETSC_COMM_WORLD,"%s\n","restart");         ierr = BreakReadMark           (actx      ); CHKERRQ(ierr); }
	else SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER," *** Incorrect option for initialization of markers");


	// compute host cells for all the markers
	ierr = ADVMapMarkToCells(actx); CHKERRQ(ierr);

	// check marker distribution
	ierr = ADVMarkCheckMarkers(actx, user); CHKERRQ(ierr);

	// project initial history from markers to grid
	ierr = ADVProjHistMarkToGrid(actx); CHKERRQ(ierr);

	PetscPrintf(PETSC_COMM_WORLD," Finished marker initialization routine\n");

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitCoord"
PetscErrorCode ADVMarkInitCoord(AdvCtx *actx, UserContext *user)
{
	//=============================================================
	// Initializes coordinates and adds random noise if required for hard-coded setups
	// HANDLES VARIABLE MESH SPACING!
	// WARNING! Random noise only for internal setups
	//==============================================================

	// generate coordinates of uniformly distributed markers within a cell
	FDSTAG      *fs;
	PetscScalar  x, y, z, dx, dy, dz;
	PetscInt     i, j, k, ii, jj, kk;
	PetscInt     imark;
	PetscRandom  rctx;
	PetscScalar  cf_rand;
	PetscInt     AddRandomNoiseParticles;


	PetscErrorCode ierr;
	PetscFunctionBegin;

	fs = actx->fs;

	// random noise
	AddRandomNoiseParticles = 0;
	ierr = PetscOptionsGetInt(PETSC_NULL,"-AddRandomNoiseParticles", &AddRandomNoiseParticles, PETSC_NULL); CHKERRQ(ierr);

	if(AddRandomNoiseParticles) PetscPrintf(PETSC_COMM_WORLD, " Adding random noise to marker distribution \n");

	// initialize the random number generator
	ierr = PetscRandomCreate(PETSC_COMM_WORLD, &rctx); CHKERRQ(ierr);
	ierr = PetscRandomSetFromOptions(rctx);            CHKERRQ(ierr);

	// marker counter
	imark = 0;

	// create uniform distribution of markers/cell for variable grid
	for(k = 0; k < fs->dsz.ncels; k++)
	{
		// spacing of particles
		dz = (fs->dsz.ncoor[k+1]-fs->dsz.ncoor[k])/user->NumPartZ;
		for(j = 0; j < fs->dsy.ncels; j++)
		{
			// spacing of particles
			dy = (fs->dsy.ncoor[j+1]-fs->dsy.ncoor[j])/user->NumPartY;
			for(i = 0; i < fs->dsx.ncels; i++)
			{
				// spacing of particles
				dx = (fs->dsx.ncoor[i+1]-fs->dsx.ncoor[i])/user->NumPartX;

				// loop over markers in cells
				for (kk = 0; kk < user->NumPartZ; kk++)
				{
					if (kk == 0) z = fs->dsz.ncoor[k] + dz*0.5;
					else         z = fs->dsz.ncoor[k] + dz*0.5 + kk*dz;

					for (jj = 0; jj < user->NumPartY; jj++)
					{
						if (jj == 0) y = fs->dsy.ncoor[j] + dy*0.5;
						else         y = fs->dsy.ncoor[j] + dy*0.5 + jj*dy;

						for (ii = 0; ii < user->NumPartX; ii++)
						{
							if (ii == 0) x = fs->dsx.ncoor[i] + dx*0.5;
							else         x = fs->dsx.ncoor[i] + dx*0.5 + ii*dx;

							// set marker coordinates
							actx->markers[imark].X[0] = x;
							actx->markers[imark].X[1] = y;
							actx->markers[imark].X[2] = z;

							if(AddRandomNoiseParticles)
							{
								// add random noise
								ierr = PetscRandomGetValueReal(rctx, &cf_rand); CHKERRQ(ierr);
								actx->markers[imark].X[0] += (cf_rand-0.5)*dx;
								ierr = PetscRandomGetValueReal(rctx, &cf_rand); CHKERRQ(ierr);
								actx->markers[imark].X[1] += (cf_rand-0.5)*dy;
								ierr = PetscRandomGetValueReal(rctx, &cf_rand); CHKERRQ(ierr);
								actx->markers[imark].X[2] += (cf_rand-0.5)*dz;
							}

							// increment local counter
							imark++;
						}
					}
				}
			}
		}
	}

	// destroy random context
	ierr = PetscRandomDestroy(&rctx); CHKERRQ(ierr);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkSave"
PetscErrorCode ADVMarkSave(AdvCtx *actx, UserContext *user)
{
	int          fd;
	PetscInt     imark;
	Marker      *P;
	char        *SaveFileName;
	PetscViewer  view_out;
	PetscScalar *markbuf, *markptr, header, chLen, chTemp, Tshift, s_nummark;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	if(!user->SaveParticles) PetscFunctionReturn(0);

	PetscPrintf(PETSC_COMM_WORLD," Saving markers in parallel to files: ./%s/%s.xxx.out \n",
		user->SaveInitialParticlesDirectory, user->ParticleFilename);

	// initialize file header for MATLAB compatibility
	header = -1;

	// create write buffer
	ierr = PetscMalloc((size_t)(5*actx->nummark)*sizeof(PetscScalar), &markbuf); CHKERRQ(ierr);

	// get characteristic length & temperature
	if(user->DimensionalUnits)
	{	chLen  = user->Characteristic.Length;
		chTemp = user->Characteristic.Temperature;
	}
	else
	{	chLen  = 1.0;
		chTemp = 1.0;
	}

	// temperature shift
	Tshift = actx->jr->scal.Tshift;

	// copy data from storage into buffer
	for(imark = 0, markptr = markbuf; imark < actx->nummark; imark++, markptr += 5)
	{
		P          =              &actx->markers[imark];
		markptr[0] =              P->X[0]*chLen;
		markptr[1] =              P->X[1]*chLen;
		markptr[2] =              P->X[2]*chLen;
		markptr[3] = (PetscScalar)P->phase;
		markptr[4] =             (P->T - Tshift)*chTemp;
	}

	// create directory
	ierr = LaMEMCreateOutputDirectory(user->SaveInitialParticlesDirectory); CHKERRQ(ierr);

	// compile file name
	asprintf(&SaveFileName, "./%s/%s.%lld.out",
		user->SaveInitialParticlesDirectory,
		user->ParticleFilename, (LLD)actx->iproc);

	// open file for binary output
	ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF, SaveFileName, FILE_MODE_WRITE, &view_out); CHKERRQ(ierr);
	ierr = PetscViewerBinaryGetDescriptor(view_out, &fd);                                    CHKERRQ(ierr);

	// write binary output
	s_nummark = (PetscScalar)actx->nummark;
	ierr = PetscBinaryWrite(fd, &header,    1,               PETSC_SCALAR, PETSC_FALSE); CHKERRQ(ierr);
	ierr = PetscBinaryWrite(fd, &s_nummark, 1,               PETSC_SCALAR, PETSC_FALSE); CHKERRQ(ierr);
	ierr = PetscBinaryWrite(fd, markbuf,    5*actx->nummark, PETSC_SCALAR, PETSC_FALSE); CHKERRQ(ierr);

	// destroy file handle & file name
	ierr = PetscViewerDestroy(&view_out); CHKERRQ(ierr);
	free(SaveFileName);

	// destroy buffer
	ierr = PetscFree(markbuf); CHKERRQ(ierr);

	PetscPrintf(PETSC_COMM_WORLD," Finished saving markers in parallel \n");

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkCheckMarkers"
PetscErrorCode ADVMarkCheckMarkers(AdvCtx *actx, UserContext *user)
{
 	// check initial marker distribution
	FDSTAG      *fs;
	PetscScalar *X;
	PetscBool    error;
	PetscScalar  xs, ys, zs;
	PetscScalar  xe, ye, ze;
	PetscInt    *numMarkCell, rbuf[4], sbuf[4];
	PetscInt     i, maxid, NumInvalidPhase, numNonLocal, numEmpty, numSparse;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	fs = actx->fs;

	// get maximum Phase
	maxid = user->num_phases - 1;

	// get local mesh sizes
	GET_DOMAIN_BOUNDS(xs, xe, fs->dsx)
	GET_DOMAIN_BOUNDS(ys, ye, fs->dsy)
	GET_DOMAIN_BOUNDS(zs, ze, fs->dsz)

	// allocate marker counter array
	ierr = makeIntArray(&numMarkCell, NULL, fs->nCells); CHKERRQ(ierr);

	// clear error flag
	error = PETSC_FALSE;

	// count markers with invalid phase ID & non-local markers
	NumInvalidPhase = 0;
	numNonLocal     = 0;

	for(i = 0; i < actx->nummark; i++)
	{
		// marker should have a valid phase ID
		if(actx->markers[i].phase > maxid) NumInvalidPhase++;

		// get marker coordinates
		X = actx->markers[i].X;

		// marker must be local (check bounding box)
		if(X[0] < xs || X[0] > xe
		|| X[1] < ys || X[1] > ye
		|| X[2] < zs || X[2] > ze) numNonLocal++;

		// count number of markers in the cells
		numMarkCell[actx->cellnum[i]]++;
	}

	// count empty & sparse cells
	numEmpty  = 0;
	numSparse = 0;

	for(i = 0; i < fs->nCells; i++)
	{
		if(numMarkCell[i] == 0) numEmpty++;
		if(numMarkCell[i] <  8) numSparse++;
	}

	// get global figures
	if(actx->nproc != 1)
	{
		sbuf[0] = NumInvalidPhase;
		sbuf[1] = numNonLocal;
		sbuf[2] = numEmpty;
		sbuf[3] = numSparse;

		ierr = MPI_Allreduce(sbuf, rbuf, 4, MPIU_INT, MPI_SUM, actx->icomm); CHKERRQ(ierr);

		NumInvalidPhase = rbuf[0];
		numNonLocal     = rbuf[1];
		numEmpty        = rbuf[2];
		numSparse       = rbuf[3];
	}

	// print diagnostics
	if(NumInvalidPhase)
	{
		ierr = PetscPrintf(PETSC_COMM_WORLD, "Number of markers that have invalid phase ID: %lld\n", (LLD)NumInvalidPhase); CHKERRQ(ierr);
		error = PETSC_TRUE;
	}

	if(numNonLocal)
	{
		ierr = PetscPrintf(PETSC_COMM_WORLD, "Number of non-local markers: %lld\n", (LLD)numNonLocal); CHKERRQ(ierr);
		error = PETSC_TRUE;
	}

	if(numEmpty)
	{
		ierr = PetscPrintf(PETSC_COMM_WORLD, "Number of exactly empty cells: %lld\n", (LLD)numEmpty); CHKERRQ(ierr);
		error = PETSC_TRUE;
	}

	if(numSparse)
	{
		ierr = PetscPrintf(PETSC_COMM_WORLD, "WARNING! Number of cells with less than 8 markers: %lld.\n", (LLD)numSparse); CHKERRQ(ierr);
	}

	if(error == PETSC_TRUE)
	{
		SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_USER, "Problems with initial marker distribution (see the above message)");
	}

	// clear
	ierr = PetscFree(numMarkCell); CHKERRQ(ierr);

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
// Specific initialization routines
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitFileParallel"
PetscErrorCode ADVMarkInitFileParallel(AdvCtx *actx, UserContext *user)
{
	//=============================================================
	// WARNING! Random noise only for internal setups!!
	// Markers from file are assumed to have the coordinates set
	//==============================================================

	int          fd;
	Marker      *P;
	PetscViewer  view_in;
	char        *LoadFileName;
	PetscScalar *markbuf, *markptr, header, chTemp, chLen, Tshift, s_nummark;
	PetscInt     imark, nummark;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	PetscPrintf(PETSC_COMM_WORLD," Loading markers in parallel from files: ./%s/%s.xxx.out \n",
		user->LoadInitialParticlesDirectory, user->ParticleFilename);

	// compile input file name
	asprintf(&LoadFileName, "./%s/%s.%lld.out",
		user->LoadInitialParticlesDirectory,
		user->ParticleFilename, (LLD)actx->iproc);

	// open file
	ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF, LoadFileName, FILE_MODE_READ, &view_in); CHKERRQ(ierr);
	ierr = PetscViewerBinaryGetDescriptor(view_in, &fd);                                   CHKERRQ(ierr);

	// read (and ignore) the silent undocumented file header
	ierr = PetscBinaryRead(fd, &header, 1, PETSC_SCALAR); CHKERRQ(ierr);

	// read number of local of markers
	ierr = PetscBinaryRead(fd, &s_nummark, 1, PETSC_SCALAR); CHKERRQ(ierr);
	nummark = (PetscInt)s_nummark;

	// allocate marker storage
	ierr = ADVReAllocStorage(actx, nummark); CHKERRQ(ierr);

	// set number of markers
	actx->nummark = nummark;

	// allocate marker buffer
	ierr = PetscMalloc((size_t)(5*actx->nummark)*sizeof(PetscScalar), &markbuf); CHKERRQ(ierr);

	// read markers into buffer
	ierr = PetscBinaryRead(fd, markbuf, 5*actx->nummark, PETSC_SCALAR); CHKERRQ(ierr);

	// destroy file handle & file name
	ierr = PetscViewerDestroy(&view_in); CHKERRQ(ierr);
	free(LoadFileName);

	// get characteristic length & temperature
	if(user->DimensionalUnits)
	{	chLen  = user->Characteristic.Length;
		chTemp = user->Characteristic.Temperature;
	}
	else
	{	chLen  = 1.0;
		chTemp = 1.0;
	}

	// temperature shift
	Tshift = actx->jr->scal.Tshift;

	// copy buffer to marker storage
	for(imark = 0, markptr = markbuf; imark < actx->nummark; imark++, markptr += 5)
	{
		P        =           &actx->markers[imark];
		P->X[0]  =           markptr[0]/chLen;
		P->X[1]  =           markptr[1]/chLen;
		P->X[2]  =           markptr[2]/chLen;
		P->phase = (PetscInt)markptr[3];
		P->T     =          (markptr[4] + Tshift)/chTemp;
	}

	// free marker buffer
	ierr = PetscFree(markbuf); CHKERRQ(ierr);

	// wait until all processors finished reading markers
	// WARNING! NOT SURE WHETHER THIS IS REALLY NECESSARY
	ierr = MPI_Barrier(PETSC_COMM_WORLD); CHKERRQ(ierr);

	PetscPrintf(PETSC_COMM_WORLD," Finished Loading markers in parallel \n");

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitFileRedundant"
PetscErrorCode ADVMarkInitFileRedundant(AdvCtx *actx, UserContext *user)
{
	//=============================================================
	// HANDLES VARIABLE MESH SPACING!
	// WARNING! Random noise only for internal setups!!
	// Markers from file are assumed to have the coordinates set
	//==============================================================

	// REDUNDANTLY loads the X,Y,Z-coord, phase and temperature data from a file
	// each processor uses its own part after loading, and ignores the rest
	// THEREFORE! not to be used for large markers files - use parallel read for that!
	// coord and temperature are assumed to be dimensional

	FDSTAG       *fs;
	int           fd;
	Marker       *P;
	PetscViewer   view_in;
	char         *LoadFileName;
	PetscScalar   maxtemp, header, chLen, chTemp, Tshift;
	PetscScalar   x, y, z, xs[3], xe[3], info[3], *phase, *temp, *xcoor, *ycoor, *zcoor;
	PetscInt      nmarkx, nmarky, nmarkz, nmark;
	PetscInt      tmarkx, tmarky, tmarkz, tmark;
	PetscInt      i, imark, maxphase;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	fs = actx->fs;

	// compile input file name
	asprintf(&LoadFileName, "./%s/%s.dat",
		user->LoadInitialParticlesDirectory,
		user->ParticleFilename);

	PetscPrintf(PETSC_COMM_WORLD," Loading markers redundantly from file: %s \n", LoadFileName);

	ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF, LoadFileName, FILE_MODE_READ, &view_in); CHKERRQ(ierr);
	ierr = PetscViewerBinaryGetDescriptor(view_in, &fd); CHKERRQ(ierr);

	// read (and ignore) the silent undocumented file header
	ierr = PetscBinaryRead(fd, &header, 1, PETSC_SCALAR); CHKERRQ(ierr);

	// read number of markers in each direction
	ierr = PetscBinaryRead(fd, info, 3, PETSC_SCALAR); CHKERRQ(ierr);

	nmarkz = (PetscInt)(info[0]);
	nmarky = (PetscInt)(info[1]);
	nmarkx = (PetscInt)(info[2]);
	nmark  = nmarkx*nmarky*nmarkz;

	// compute total number of markers
	tmarkx = fs->dsx.tcels*user->NumPartX;
	tmarky = fs->dsy.tcels*user->NumPartY;
	tmarkz = fs->dsz.tcels*user->NumPartZ;
	tmark  = tmarkx*tmarky*tmarkz;

	// check whether file contains the expected number of markers
	if(tmark != nmark)
	{
		SETERRQ2(PETSC_COMM_WORLD,PETSC_ERR_USER," The file does not contain the expected number of markers [expected = %lld vs. present = %lld]", (LLD)tmark, (LLD)nmark);
	}

	// create buffer for x,y,z, phase & temperature
	ierr = PetscMalloc((size_t)nmark*sizeof(PetscScalar), &xcoor); CHKERRQ(ierr);
	ierr = PetscMalloc((size_t)nmark*sizeof(PetscScalar), &ycoor); CHKERRQ(ierr);
	ierr = PetscMalloc((size_t)nmark*sizeof(PetscScalar), &zcoor); CHKERRQ(ierr);
	ierr = PetscMalloc((size_t)nmark*sizeof(PetscScalar), &phase); CHKERRQ(ierr);
	ierr = PetscMalloc((size_t)nmark*sizeof(PetscScalar), &temp);  CHKERRQ(ierr);

	// read phase and temperature into buffer
	ierr = PetscBinaryRead(fd, xcoor, nmark, PETSC_SCALAR); CHKERRQ(ierr);
	ierr = PetscBinaryRead(fd, ycoor, nmark, PETSC_SCALAR); CHKERRQ(ierr);
	ierr = PetscBinaryRead(fd, zcoor, nmark, PETSC_SCALAR); CHKERRQ(ierr);
	ierr = PetscBinaryRead(fd, phase, nmark, PETSC_SCALAR); CHKERRQ(ierr);
	ierr = PetscBinaryRead(fd, temp , nmark, PETSC_SCALAR); CHKERRQ(ierr);

	// destroy file-handle & file name
	ierr = PetscViewerDestroy(&view_in); CHKERRQ(ierr);
	free(LoadFileName);

	// fill markers array
	imark    = 0;
	maxphase = 0;
	maxtemp  = 0.0;

	// coordinates of the local domain
	xs[0] = fs->dsx.ncoor[0];
	xs[1] = fs->dsy.ncoor[0];
	xs[2] = fs->dsz.ncoor[0];

	xe[0] = fs->dsx.ncoor[fs->dsx.ncels];
	xe[1] = fs->dsy.ncoor[fs->dsy.ncels];
	xe[2] = fs->dsz.ncoor[fs->dsz.ncels];

	// set characteristic length and temperature
	if(user->DimensionalUnits)
	{	chLen  = user->Characteristic.Length;
		chTemp = user->Characteristic.Temperature;
	}
	else
	{	chLen  = 1.0;
		chTemp = 1.0;
	}

	// temperature shift
	Tshift = actx->jr->scal.Tshift;

	// loop over all markers and put them in the local domain
	for(i = 0; i < nmark; i++)
	{
		x  = xcoor[i]/chLen;
		y  = ycoor[i]/chLen;
		z  = zcoor[i]/chLen;

		// truncate non-local markers
		if(x >= xs[0] && x <= xe[0]
		&& y >= xs[1] && y <= xe[1]
		&& z >= xs[2] && z <= xe[2])
		{
			P        = &actx->markers[imark];
			P->X[0]  = x;
			P->X[1]  = y;
			P->X[2]  = z;
			P->phase = (PetscInt)phase[i];
			P->T     = (temp[i] + Tshift)/chTemp;

			// increment local counter
			imark++;
		}

		// calculate max no of phases
		if((PetscInt)phase[i] > maxphase)
		{
			maxphase = (PetscInt) phase[i];
		}

		// calculate max temperature
		if(temp[i] > maxtemp)
		{
			maxtemp = temp[i];
		}
	}

	// error checking
	if((maxphase + 1) != user->num_phases)
	{
		SETERRQ2(PETSC_COMM_WORLD,PETSC_ERR_USER," No. of detected phases %lld does not correspond to the no. of phases given in parameters file %lld", (LLD)maxphase, (LLD)user->num_phases);
	}

	PetscPrintf(PETSC_COMM_WORLD," Statistics markers: MaxPhase = %lld, MaxTemp = %g, NumberPhases = %lld \n", (LLD)maxphase, maxtemp, (LLD)user->num_phases);

	// free specific allocated memory
	ierr = PetscFree(xcoor); CHKERRQ(ierr);
	ierr = PetscFree(ycoor); CHKERRQ(ierr);
	ierr = PetscFree(zcoor); CHKERRQ(ierr);
	ierr = PetscFree(phase); CHKERRQ(ierr);
	ierr = PetscFree(temp);  CHKERRQ(ierr);

	// wait until all processors finished reading markers
	// WARNING! NOT SURE WHETHER THIS IS REALLY NECESSARY
	ierr = MPI_Barrier(PETSC_COMM_WORLD); CHKERRQ(ierr);

	PetscPrintf(PETSC_COMM_WORLD," Finished Loading markers redundantly \n");

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitDiapir"
PetscErrorCode ADVMarkInitDiapir(AdvCtx *actx, UserContext *user)
{
	PetscInt imark;

	PetscFunctionBegin;

	// Diapir setup - not done - should have some perturbation

	// print info
	PetscPrintf(PETSC_COMM_WORLD," DIAPIR SETUP\n");
	PetscPrintf(PETSC_COMM_WORLD," Setup Parameters: Setup.Diapir_Hi = [%g]\n",user->Setup.Diapir_Hi);

	for(imark = 0; imark < actx->nummark; imark++)
	{
		// phase
		if(actx->markers[imark].X[2] >= user->Setup.Diapir_Hi) actx->markers[imark].phase = 1;
		else                                                   actx->markers[imark].phase = 0;

		// temperature
		actx->markers[imark].T = 0.0;
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitBlock"
PetscErrorCode ADVMarkInitBlock(AdvCtx *actx, UserContext *user)
{
	// falling block setup

	PetscInt    imark, nel_x, nel_y, nel_z;
	PetscScalar dx,dy,dz;
	PetscScalar bleft, bright, bfront, bback, bbottom, btop;
	PetscScalar blx, bly, blz;
	PetscBool   b2D, b2Dy;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  FALLING BLOCK SETUP \n");

	// number of elements on finest resolution
	nel_x = user->nel_x;
	nel_y = user->nel_y;
	nel_z = user->nel_z;

	// spacing
	dx = user->W/((PetscScalar)nel_x);
	dy = user->L/((PetscScalar)nel_y);
	dz = user->H/((PetscScalar)nel_z);

	// block dimensions
	blx = ((PetscScalar) (nel_x*0.5))*dx;
	bly = ((PetscScalar) (nel_y*0.5))*dy;
	blz = ((PetscScalar) (nel_z*0.5))*dz;

	bleft   = ((PetscScalar) (nel_x*0.25))*dx + user->x_left ; bright = bleft   + blx; // left and right side of block
	bfront  = ((PetscScalar) (nel_y*0.25))*dy + user->y_front; bback  = bfront  + bly; // front and back side of block
	bbottom = ((PetscScalar) (nel_z*0.25))*dz + user->z_bot  ; btop   = bbottom + blz; // bottom and top side of block

	PetscPrintf(PETSC_COMM_WORLD,"      Block coordinates: [left,right]=[%g,%g]; \n",bleft,bright);
    PetscPrintf(PETSC_COMM_WORLD,"                         [front,back]=[%g,%g]; \n",bfront,bback);
    PetscPrintf(PETSC_COMM_WORLD,"                         [bottom,top]=[%g,%g]; \n",bbottom,btop);
    

	b2D  = PETSC_FALSE;
	b2Dy = PETSC_FALSE;
	ierr = PetscOptionsGetBool( PETSC_NULL, "-Block_2D" , &b2D , PETSC_NULL); CHKERRQ(ierr);
	ierr = PetscOptionsGetBool( PETSC_NULL, "-Block_2Dy", &b2Dy, PETSC_NULL); CHKERRQ(ierr);

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = actx->markers[imark].X[2];

		if((actx->markers[imark].X[2] > bbottom)
		&& (actx->markers[imark].X[2] < btop)
		&& (actx->markers[imark].X[0] > bleft)
		&& (actx->markers[imark].X[0] < bright)
		&& (b2Dy == PETSC_FALSE))
		{
			if(b2D == PETSC_TRUE)
			{
				// 2D block
				actx->markers[imark].phase = 1;
				actx->markers[imark].T     = 1.0;
			}
			else
			{
				if((actx->markers[imark].X[1] > bfront)
				&& (actx->markers[imark].X[1] < bback))
				{
					// 3D block
					actx->markers[imark].phase = 1;
					actx->markers[imark].T     = 1.0;
				}
			}
		}
		if(b2Dy == PETSC_TRUE)
		{
			// create 2D block in y-direction
			if((actx->markers[imark].X[2] > bbottom)
			&& (actx->markers[imark].X[2] < btop)
			&& (actx->markers[imark].X[1] > bfront)
			&& (actx->markers[imark].X[1] < bback))
			{
				// 2D block
				actx->markers[imark].phase = 1;
				actx->markers[imark].T     = 1.0;
			}
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitSubduction"
PetscErrorCode ADVMarkInitSubduction(AdvCtx *actx, UserContext *user)
{
	// subduction setup with air

	PetscScalar H_air, SlabThickness, SlabWidth, SlabLength;
	PetscScalar DistanceFromLeft, SlabMaxSubdDepth,Slab_Xright, dz;
	PetscScalar Air_thickness, Slab_ThicknessFactor, Air_ThicknessFactor, Slab_WidthFactor;
	PetscInt    imark, nz;

	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  SUBDUCTION WITH STICKY AIR SETUP \n");

	// TOTAL number of CELLS and spacing in z-direction
	nz = user->nel_z;
	dz = user->H/((PetscScalar)nz);

	// read some input parameters
	Slab_ThicknessFactor = 0.1; // in % of the total H
	PetscOptionsGetReal(PETSC_NULL ,"-Slab_ThicknessFactor", &Slab_ThicknessFactor, PETSC_NULL);

	Air_ThicknessFactor  = 0.1; // in % of the total H
	PetscOptionsGetReal(PETSC_NULL ,"-Air_ThicknessFactor" , &Air_ThicknessFactor , PETSC_NULL);

	Slab_WidthFactor     = 1.0;
	PetscOptionsGetReal(PETSC_NULL ,"-Slab_WidthFactor"    , &Slab_WidthFactor    , PETSC_NULL);

	// setup parameters
	SlabThickness = (PetscScalar) (round(user->H/dz)*dz*Slab_ThicknessFactor);
	Air_thickness = (PetscScalar) (round(user->H/dz)*dz*Air_ThicknessFactor );
	H_air         = (PetscScalar) (user->z_bot + user->H - Air_thickness    );

	SlabWidth        = Slab_WidthFactor*user->L;
	SlabLength       = 0.4 * (user->W);
	DistanceFromLeft = (user->W) * 0.1;
	SlabMaxSubdDepth = 2 * SlabThickness;
	Slab_Xright      = DistanceFromLeft + SlabLength-(user->H-H_air);

	PetscPrintf(PETSC_COMM_WORLD," Setup Parameters: SlabThickness = [%g km] AirThickness = [%g km] H = [%g km] SlabWidthFactor = [%g] \n",SlabThickness*user->Characteristic.Length*0.001, Air_thickness*user->Characteristic.Length*0.001, user->H*user->Characteristic.Length*0.001, Slab_WidthFactor);

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = 0.0;

		if((actx->markers[imark].X[2] > (H_air - SlabThickness))
		&& (actx->markers[imark].X[2] <  H_air)
		&& (actx->markers[imark].X[0] > (user->x_left  + DistanceFromLeft))
		&& (actx->markers[imark].X[0] < (user->x_left  + DistanceFromLeft + SlabLength))
		&& (actx->markers[imark].X[1] < (user->y_front + SlabWidth)))
		{
			actx->markers[imark].phase = 1; // slab flat part
		}

		if((actx->markers[imark].X[2] < (user->H - (actx->markers[imark].X[0]-Slab_Xright)))
		&& (actx->markers[imark].X[2] > (user->H-2*SlabThickness - (actx->markers[imark].X[0]-Slab_Xright)))
		&& (actx->markers[imark].X[1] < (user->y_front+SlabWidth))
		&& (actx->markers[imark].X[2] > (H_air-SlabMaxSubdDepth)))
		{
			actx->markers[imark].phase = 1; // slab inclined part
		}

		if (actx->markers[imark].X[2] > H_air)
		{
			actx->markers[imark].phase = 2; // air
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitFolding"
PetscErrorCode ADVMarkInitFolding(AdvCtx *actx, UserContext *user)
{
	// multilayer folding setup (Zagros)

	PetscInt    i, imark;
	PetscBool   flg, DisplayInfo;
	PetscScalar zbot[10], ztop[10];

	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  MULTILAYER FOLDING SETUP \n");

	// initialize arrays for fractions
	for(i = 0;i < 10; i++) { zbot[i] = 0.0; ztop[i] = 0.0; }

	// get input data
	DisplayInfo = PETSC_TRUE;

	PetscOptionsGetReal(PETSC_NULL,"-Layer1_bottom"      ,&zbot[0], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer1_top"         ,&ztop[0], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=1 from z=[%g,%g]\n",zbot[0],ztop[0]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer2_bottom"      ,&zbot[1], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer2_top"         ,&ztop[1], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=2 from z=[%g,%g]\n",zbot[1],ztop[1]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer3_bottom"      ,&zbot[2], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer3_top"         ,&ztop[2], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=3 from z=[%g,%g]\n",zbot[2],ztop[2]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer4_bottom"      ,&zbot[3], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer4_top"         ,&ztop[3], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=4 from z=[%g,%g]\n",zbot[3],ztop[3]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer5_bottom"      ,&zbot[4], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer5_top"         ,&ztop[4], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=5 from z=[%g,%g]\n",zbot[4],ztop[4]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer6_bottom"      ,&zbot[5], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer6_top"         ,&ztop[5], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=6 from z=[%g,%g]\n",zbot[5],ztop[5]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer7_bottom"      ,&zbot[6], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer7_top"         ,&ztop[6], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=7 from z=[%g,%g]\n",zbot[6],ztop[6]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer8_bottom"      ,&zbot[7], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer8_top"         ,&ztop[7], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=8 from z=[%g,%g]\n",zbot[7],ztop[7]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer9_bottom"      ,&zbot[8], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer9_top"         ,&ztop[8], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=9 from z=[%g,%g]\n",zbot[8],ztop[8]);

	PetscOptionsGetReal(PETSC_NULL,"-Layer10_bottom"      ,&zbot[9], PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer10_top"         ,&ztop[9], &flg);
	if (DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase=10 from z=[%g,%g]\n",zbot[9],ztop[9]);

	// transform fractions into domain coordinates
	for(i = 0; i < 10; i++) { zbot[i] = zbot[i]*user->H; ztop[i] = ztop[i]*user->H;}

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = 0.0;

		for(i = 0; i < 10; i++)
		{
			if((actx->markers[imark].X[2] >= zbot[i])
			&& (actx->markers[imark].X[2] < ztop[i]))
			{
				actx->markers[imark].phase = i+1;
			}
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitDetachment"
PetscErrorCode ADVMarkInitDetachment(AdvCtx *actx, UserContext *user)
{
	// 1-layer over detachment with two linear shaped perturbation (Grasemann & Schmalholz 2012)
	// one perturbation is fixed in position, the other one's position is varied with -Heterogeneity_Offset

	PetscInt    imark;
	PetscScalar zbot = 0.0, ztop = 0.0;
	PetscScalar Het_L, Het_W, Het_A, Het_Off;
	PetscBool   flg, DisplayInfo;

	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  ONE-LAYER OVER DETACHMENT WITH 2 LINEAR PERTURBATIONS SETUP \n");

	DisplayInfo = PETSC_TRUE;
	PetscOptionsGetReal(PETSC_NULL,"-Heterogeneity_L"      , &Het_L  , PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Heterogeneity_W"      , &Het_W  , PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Heterogeneity_A"      , &Het_A  , PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Heterogeneity_Offset" , &Het_Off, PETSC_NULL);

	PetscOptionsGetReal(PETSC_NULL,"-Layer1_bottom", &zbot, PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL,"-Layer1_top"   , &ztop, &flg);
	if(DisplayInfo && flg) PetscPrintf(PETSC_COMM_WORLD," - Adding Layer with Phase = 1 from z=[%g,%g]\n", zbot, ztop);

	// transform fractions into domain coordinates
	zbot = zbot*user->H; ztop = ztop*user->H;

	// non-dimensionalization
	Het_L   = Het_L  /user->Characteristic.Length;
	Het_W   = Het_W  /user->Characteristic.Length;
	Het_A   = Het_A  /user->Characteristic.Length;
	Het_Off = Het_Off/user->Characteristic.Length;

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = 0.0;

		// one layer properties
		if((actx->markers[imark].X[2] >= zbot)
		&& (actx->markers[imark].X[2] <  ztop))
		{
			actx->markers[imark].phase = 1;
		}

		// seed fixed
		if((actx->markers[imark].X[1] >= (user->L*0.5 - Het_L*0.5))
		&& (actx->markers[imark].X[1] <= (user->L*0.5 + Het_L*0.5))
		&& (actx->markers[imark].X[0] <= (Het_W))
		&& (actx->markers[imark].X[2] <= (zbot + Het_A)))
		{
			actx->markers[imark].phase = 0;
		}

		// seed with offset
		if((actx->markers[imark].X[1] >= (Het_Off + user->L*0.5 - Het_L*0.5))
		&& (actx->markers[imark].X[1] <= (Het_Off + user->L*0.5 + Het_L*0.5))
		&& (actx->markers[imark].X[0] >= (user->W - Het_W))
		&& (actx->markers[imark].X[2] <= (zbot    + Het_A)))
		{
			actx->markers[imark].phase = 0;
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitSlab"
PetscErrorCode ADVMarkInitSlab(AdvCtx *actx, UserContext *user)
{
	// slab detachment (Thieulot et al. 2014)
	// domain parameters should be given in params file because they are needed to create the grid
	// W = 1000.0e3; L = 500.0e3; H = 700.0e3; ThicknessAir = 40.0e3;

	PetscInt imark;

	// input parameters - chosen as function of L (y-dir)
	PetscScalar hslab = 0.16; // thickness of slab L = 80 km
	PetscScalar lslab = 0.5;  // length of slab    L = 250 km
	PetscScalar dslab = 0.5;  // depth of slab     L = 250 km
	PetscScalar dair  = 0.08; // thickness air     L = 40 km

	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  SLAB DETACHMENT SETUP \n");

	// non-dimensionalization
	hslab = hslab*user->L;
	lslab = lslab*user->L;
	dslab = dslab*user->L;
	dair  = dair *user->L;

	PetscPrintf(PETSC_COMM_WORLD," Setup Parameters: W = [%g km] L = [%g km] H = [%g km] SlabDimensions = [%g km,%g km,%g km] DepthSlab = [%g km] ThicknessAir = [%g km] \n",user->W*user->Characteristic.Length/1000,user->L*user->Characteristic.Length/1000,user->H*user->Characteristic.Length/1000, hslab*user->Characteristic.Length/1000,lslab*user->Characteristic.Length/1000,dslab*user->Characteristic.Length/1000,dslab*user->Characteristic.Length/1000,dair*user->Characteristic.Length/1000);

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		// initialize
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = 0.0;

		// flat slab
		if(actx->markers[imark].X[2] >= (user->H-hslab - dair)
		&& actx->markers[imark].X[2] <= (user->H-dair))
		{
			actx->markers[imark].phase = 1;
		}
		else
		{	// vertical slab
			if(actx->markers[imark].X[0] >= (user->W/2.0 - hslab/2.0)
			&& actx->markers[imark].X[0] <= (user->W/2.0 + hslab/2.0)
			&& actx->markers[imark].X[1] <= lslab
			&& actx->markers[imark].X[2] >= (user->H - hslab - dslab - dair)
			&& actx->markers[imark].X[2] <= (user->H - hslab - dair))
			{
				actx->markers[imark].phase = 1;
			}
		}

		// sticky air layer
		if(actx->markers[imark].X[2] >= (user->H - dair))
		{
			actx->markers[imark].phase = 2;
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitSpheres"
PetscErrorCode ADVMarkInitSpheres(AdvCtx *actx, UserContext *user)
{
	// multiple falling spheres

	PetscInt    nsphere = 10, ind, imark;
	PetscScalar rsphere = 0.1;
	PetscScalar x = 0.0, y = 0.0, z = 0.0;
	PetscScalar xc[20], yc[20], zc[20];

	if(user) user = NULL;

 	PetscFunctionBegin;

	// print info
	PetscPrintf(PETSC_COMM_WORLD,"  MULTIPLE FALLING SPHERES SETUP \n");

	// get data
	PetscOptionsGetInt (PETSC_NULL, "-NumSpheres",   &nsphere, PETSC_NULL);
	PetscOptionsGetReal(PETSC_NULL, "-SphereRadius", &rsphere, PETSC_NULL);

	// distribution - set for now such that the spheres do not touch
	xc[0] = 0.25; yc[0] = 0.15; zc[0] = 0.30;
	xc[1] = 0.15; yc[1] = 0.65; zc[1] = 0.30;
	xc[2] = 0.25; yc[2] = 0.55; zc[2] = 0.70;
	xc[3] = 0.35; yc[3] = 0.25; zc[3] = 0.70;
	xc[4] = 0.45; yc[4] = 0.35; zc[4] = 0.30;
	xc[5] = 0.35; yc[5] = 0.65; zc[5] = 0.50;
	xc[6] = 0.65; yc[6] = 0.15; zc[6] = 0.50;
	xc[7] = 0.55; yc[7] = 0.45; zc[7] = 0.50;
	xc[8] = 0.65; yc[8] = 0.35; zc[8] = 0.70;
	xc[9] = 0.55; yc[9] = 0.65; zc[9] = 0.30;

	// loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		// initialize
		actx->markers[imark].phase = 0;
		actx->markers[imark].T     = 0.0;

		// spheres
		for(ind = 0; ind < nsphere; ind++)
		{
			x = actx->markers[imark].X[0] - xc[ind];
			y = actx->markers[imark].X[1] - yc[ind];
			z = actx->markers[imark].X[2] - zc[ind];

			if(x*x + y*y + z*z <= rsphere*rsphere)
			{
				actx->markers[imark].phase = 1;
			}
		}
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
#undef __FUNCT__
#define __FUNCT__ "ADVMarkInitBands"
PetscErrorCode ADVMarkInitBands(AdvCtx *actx, UserContext *user)
{
	Marker     *P;
	PetscInt    imark;
	PetscBool   use_inc;
	PetscScalar H, Hb, size, offset, length, x, y, z, scal;

	PetscErrorCode ierr;
	PetscFunctionBegin;

	scal = actx->jr->scal.length;

	// set default values
	H      = 10.0;               // layer thickness               [km]
    Hb     = 2.0;                // basal layer thickness         [km]
    size   = 1.0;                // inclusion size in XZ-plane    [km]
    offset = 0.0;                // inclusion offset along X axis [km]
    length = (user->L*scal)/2.0; // inclusion length along Y axis [km]
    
    // get layer thickness
    ierr = PetscOptionsGetReal(PETSC_NULL, "-H_layer",  &H,  PETSC_NULL);  CHKERRQ(ierr);
    ierr = PetscOptionsGetReal(PETSC_NULL, "-H_bottom", &Hb, PETSC_NULL);  CHKERRQ(ierr);

    // get inclusion parameters
    ierr = PetscOptionsHasName(PETSC_NULL, "-Inclusion_use", &use_inc); CHKERRQ(ierr);
    if(use_inc == PETSC_TRUE)
    {
    	ierr = PetscOptionsGetReal(PETSC_NULL, "-Inclusion_size",   &size,   PETSC_NULL); CHKERRQ(ierr);
    	ierr = PetscOptionsGetReal(PETSC_NULL, "-Inclusion_offset", &offset, PETSC_NULL); CHKERRQ(ierr);
    	ierr = PetscOptionsGetReal(PETSC_NULL, "-Inclusion_length", &length, PETSC_NULL); CHKERRQ(ierr);
    }

	// scale
	H      /= scal;
    Hb     /= scal;
	size   /= scal;
    offset /= scal;
    length /= scal;

    // loop over local markers
	for(imark = 0; imark < actx->nummark; imark++)
	{
		P = &actx->markers[imark];

		// get coordinates
		x = P->X[0];
		y = P->X[1];
		z = P->X[2];

		// assign phase in layers
		if     (z >  Hb + H)            P->phase = 2; // air
		else if(z >= Hb && z <= Hb + H) P->phase = 1; // matrix
		else                            P->phase = 0; // basal layer

		// check inclusion
		if(use_inc == PETSC_TRUE && z >= Hb && z <= Hb + size
		&& ((y <= length         && x >= -(offset + size)/2.0 && x <= -(offset - size)/2.0)
		||  (y >= user->L-length && x >=  (offset - size)/2.0 && x <=  (offset + size)/2.0))) P->phase = 0;

		// assign temperature
		P->T = 0.0;
	}

	PetscFunctionReturn(0);
}
//---------------------------------------------------------------------------
