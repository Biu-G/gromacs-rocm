/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * GROwing Monsters And Cloning Shrimps
 */
/* IMPORTANT FOR DEVELOPERS:
 *
 * Triclinic pme stuff isn't entirely trivial, and we've experienced
 * some bugs during development (many of them due to me). To avoid
 * this in the future, please check the following things if you make
 * changes in this file:
 *
 * 1. You should obtain identical (at least to the PME precision)
 *    energies, forces, and virial for
 *    a rectangular box and a triclinic one where the z (or y) axis is
 *    tilted a whole box side. For instance you could use these boxes:
 *
 *    rectangular       triclinic
 *     2  0  0           2  0  0
 *     0  2  0           0  2  0
 *     0  0  6           2  2  6
 *
 * 2. You should check the energy conservation in a triclinic box.
 *
 * It might seem an overkill, but better safe than sorry.
 * /Erik 001109
 */ 

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <stdio.h>
#include <string.h>
#include <math.h>
#include "typedefs.h"
#include "txtdump.h"
#include "vec.h"
#include "gmxcomplex.h"
#include "smalloc.h"
#include "futil.h"
#include "coulomb.h"
#include "fftgrid.h"
#include "gmx_fatal.h"
#include "pme.h"
#include "network.h"
#include "physics.h"
#include "nrnb.h"
#include "copyrite.h"
#ifdef GMX_MPI
#include <mpi.h>
#endif
#include "mpelogging.h"

#define DFT_TOL 1e-7
/* #define PRT_FORCE */
/* conditions for on the fly time-measurement */
//#define TAKETIME (step > 1 && timesteps < 10)
#define TAKETIME FALSE

#ifdef GMX_DOUBLE
#define mpi_type MPI_DOUBLE
#else
#define mpi_type MPI_FLOAT
#endif

#ifdef GMX_MPI
MPI_Datatype  rvec_mpi;
#endif

/* Internal datastructures */
typedef struct {
  int atom,index;
} t_pmeatom;

typedef struct gmx_pme {
  bool bPar;               /* Run in parallel? */
  bool bFEP;               /* Compute Free energy contribution */
  bool bVerbose;           /* Should we be talkative? */
  int leftbnd, rightbnd;   /* Size of left and right boundary */
  int my_homenr;
  int nkx,nky,nkz;         /* Grid dimensions */
  int pme_order;
  real epsilon_r;           
  t_fftgrid *gridA,*gridB;
  int  *nnx,*nny,*nnz;
  ivec *idx;
  rvec *fractx;            /* Fractional coordinate relative to the
			    * lower cell boundary 
			    */
  matrix    recipbox;
  splinevec theta,dtheta,bsp_mod;
  t_pmeatom *pmeatom;
} t_gmx_pme;

typedef struct {
  int    natoms;
  matrix box;
  bool   bLastStep;
} gmx_pme_comm_n_box_t;

typedef struct {
  matrix vir;
  real   energy;
  int    flags;
} gmx_pme_comm_vir_ene_t;

/* The following stuff is needed for signal handling on the PME nodes. 
 * signal_handler needs to be defined in md.c, the bGot..Signal variables
 * here */ 
extern RETSIGTYPE signal_handler(int n);

volatile bool bGotTermSignal = FALSE, bGotUsr1Signal = FALSE; 

#define PME_TERM (1<<0)
#define PME_USR1 (1<<1)

/* #define SORTPME */

static void pr_grid_dist(FILE *fp,char *title,t_fftgrid *grid)
{
  int     i,j,k,l,ntoti,ntot=0;
  int     nx,ny,nz,nx2,ny2,nz2,la12,la2;
  real *  ptr;

  /* Unpack structure */
  unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
  for(i=0; (i<nx); i++) {
    ntoti=0;
    for(j=0; (j<ny); j++)
      for(k=0; (k<nz); k++) {
        l= INDEX(i,j,k);
        if (ptr[l] != 0) {
          ntoti++;
          ntot++;
        }
      }
          fprintf(fp,"%-12s  %5d  %5d\n",title,i,ntoti);
    }
  fprintf(fp,"%d non zero elements in %s\n",ntot,title);
}
/* test */

static void calc_recipbox(matrix box,matrix recipbox)
{
  /* Save some time by assuming upper right part is zero */

  real tmp=1.0/(box[XX][XX]*box[YY][YY]*box[ZZ][ZZ]);

  recipbox[XX][XX]=box[YY][YY]*box[ZZ][ZZ]*tmp;
  recipbox[XX][YY]=0;
  recipbox[XX][ZZ]=0;
  recipbox[YY][XX]=-box[YY][XX]*box[ZZ][ZZ]*tmp;
  recipbox[YY][YY]=box[XX][XX]*box[ZZ][ZZ]*tmp;
  recipbox[YY][ZZ]=0;
  recipbox[ZZ][XX]=(box[YY][XX]*box[ZZ][YY]-box[YY][YY]*box[ZZ][XX])*tmp;
  recipbox[ZZ][YY]=-box[ZZ][YY]*box[XX][XX]*tmp;
  recipbox[ZZ][ZZ]=box[XX][XX]*box[YY][YY]*tmp;
}

static int comp_pmeatom(const void *a,const void *b)
{
  t_pmeatom *pa,*pb;
  pa=(t_pmeatom *)a;
  pb=(t_pmeatom *)b;
  
  return pa->index-pb->index;
}

static void calc_idx(gmx_pme_t pme,rvec x[])
{
  int  i;
  int  *idxptr,tix,tiy,tiz;
  real *xptr,*fptr,tx,ty,tz;
  real rxx,ryx,ryy,rzx,rzy,rzz;
  int  nx,ny,nz,nx2,ny2,nz2,la12,la2;
  real *ptr;

#if (defined __GNUC__ && (defined i386 || defined __386__) && !defined GMX_DOUBLE && defined GMX_X86TRUNC)
  int x86_cw,x86_cwsave;

  asm("fnstcw %0" : "=m" (*&x86_cwsave));
  x86_cw = x86_cwsave | 3072;
  asm("fldcw %0" : : "m" (*&x86_cw));
  #define x86trunc(a,b) asm("fld %1\nfistpl %0\n" : "=m" (*&b) : "f" (a));
#endif
 
  /* Unpack structure */
  unpack_fftgrid(pme->gridA,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
  
  rxx = pme->recipbox[XX][XX];
  ryx = pme->recipbox[YY][XX];
  ryy = pme->recipbox[YY][YY];
  rzx = pme->recipbox[ZZ][XX];
  rzy = pme->recipbox[ZZ][YY];
  rzz = pme->recipbox[ZZ][ZZ];

  for(i=0; (i<pme->my_homenr); i++) {
    xptr   = x[i];
    idxptr = pme->idx[i];
    fptr   = pme->fractx[i];
    
    /* Fractional coordinates along box vectors */
    tx = nx2 + nx * ( xptr[XX] * rxx + xptr[YY] * ryx + xptr[ZZ] * rzx );
    ty = ny2 + ny * (                  xptr[YY] * ryy + xptr[ZZ] * rzy );
    tz = nz2 + nz * (                                   xptr[ZZ] * rzz );
    
#if (defined __GNUC__ && (defined i386 || defined __386__) && !defined GMX_DOUBLE && defined GMX_X86TRUNC)
    x86trunc(tx,tix);
    x86trunc(ty,tiy);
    x86trunc(tz,tiz);
#else
    tix = (int)(tx);
    tiy = (int)(ty);
    tiz = (int)(tz);
#endif

    fptr[XX] = tx - tix;
    fptr[YY] = ty - tiy;
    fptr[ZZ] = tz - tiz;   
    
    idxptr[XX] = pme->nnx[tix];
    idxptr[YY] = pme->nny[tiy];
    idxptr[ZZ] = pme->nnz[tiz];

#ifdef SORTPME    
    pme->pmeatom[i].atom  = i;
    pme->pmeatom[i].index = INDEX(idxptr[XX],idxptr[YY],idxptr[ZZ]);
#endif
    
#ifdef DEBUG
    range_check(idxptr[XX],0,nx);
    range_check(idxptr[YY],0,ny);
    range_check(idxptr[ZZ],0,nz);
#endif
  }  
#if (defined __GNUC__ && (defined i386 || defined __386__) && !defined GMX_DOUBLE && defined GMX_X86TRUNC)  
  asm("fldcw %0" : : "m" (*&x86_cwsave));
#endif

#ifdef SORTPME
  GMX_MPE_LOG(ev_sort_start);
  qsort(pme->pmeatom,pme->my_homenr,sizeof(pmeatom[0]),comp_pmeatom);
  GMX_MPE_LOG(ev_sort_finish);
#endif
}

static void pme_calc_pidx(t_commrec *cr,
			  int natoms,matrix box, rvec x[],int nx, int nnx[],
			  int *pidx, int *gidx, int *count)
{
  int  i,j,jj;
  int nx2;
  int  tix;
  int  irange, taskid;
  real *xptr,tx;
  real rxx,ryx,rzx;
  matrix recipbox;
  int  npme; /* how many nodes participate in PME? */

  if (cr->npmenodes)
    npme = cr->npmenodes;
  else 
    npme = cr->nnodes;
    
/* Calculate PME task index (pidx) for each grid index */
  irange=1+(nx-1)/npme;

  taskid=0;
  for(i=0; i<nx; i+=irange) {
    jj=min((i+irange),nx);
    for(j=i; (j<jj); j++) pidx[j]=taskid;
    taskid++;
  }

  if (count)
    for(i=0; i<npme; i++)
      count[i] = 0;

  nx2=2*nx;
  calc_recipbox(box,recipbox);
  rxx = recipbox[XX][XX];
  ryx = recipbox[YY][XX];
  rzx = recipbox[ZZ][XX];
/* Calculate grid index in x-dimension */
  for(i=0; (i<natoms); i++) {
    xptr   = x[i];
    /* Fractional coordinates along box vectors */
    tx = nx2 + nx * ( xptr[XX] * rxx + xptr[YY] * ryx + xptr[ZZ] * rzx );
    tix = (int)(tx);
    gidx[i] = pidx[nnx[tix]];
    if (count)
      count[gidx[i]]++;
  }
}

static void pmeredist(t_commrec *cr, bool forw,
		      int n, rvec *x_f, real *charge, int *idxa,
		      int *total, rvec *pme_x, real *pme_q)
/* Redistribute particle data for PME calculation */
/* domain decomposition by x coordinate           */
{
  static int *scounts, *rcounts,*sdispls, *rdispls, *sidx;
  static real *buf=NULL;
  static int buf_nalloc=0;
  int i, ii;
  static bool bFirst=TRUE;
  static int  npme; /* how many nodes participate in PME? */

  if(bFirst) {
    if (cr->npmenodes)
      npme = cr->npmenodes;
    else 
      npme = cr->nnodes;

    snew(scounts,npme);
    snew(rcounts,npme);
    snew(sdispls,npme);
    snew(rdispls,npme);
    snew(sidx,npme);
    bFirst=FALSE;
  }
  if (n > buf_nalloc) {
    buf_nalloc = over_alloc(n);
    srenew(buf,buf_nalloc*DIM);
  }

#ifdef GMX_MPI
  if (forw) { /* forward, redistribution from pp to pme */ 

/* Calculate send counts and exchange them with other nodes */
    for(i=0; (i<npme); i++) scounts[i]=0;
    for(i=0; (i<n); i++) scounts[idxa[i]]++;
    MPI_Alltoall( scounts, 1, MPI_INT, rcounts, 1, MPI_INT, cr->mpi_comm_mygroup );

/* Calculate send and receive displacements and index into send buffer */
    sdispls[0]=0;
    rdispls[0]=0;
    sidx[0]=0;
    for(i=1; i<npme; i++) {
      sdispls[i]=sdispls[i-1]+scounts[i-1];
      rdispls[i]=rdispls[i-1]+rcounts[i-1];
      sidx[i]=sdispls[i];
    }
/* Total # of particles to be received */
    *total=rdispls[npme-1]+rcounts[npme-1];

/* Copy particle coordinates into send buffer and exchange*/
    for(i=0; (i<n); i++) {
      ii=DIM*sidx[idxa[i]];
      sidx[idxa[i]]++;
      buf[ii+XX]=x_f[i][XX];
      buf[ii+YY]=x_f[i][YY];
      buf[ii+ZZ]=x_f[i][ZZ];
    }
    MPI_Alltoallv(buf, scounts, sdispls, rvec_mpi,
                  pme_x, rcounts, rdispls, rvec_mpi,
                  cr->mpi_comm_mygroup);

/* Copy charge into send buffer and exchange*/
    for(i=0; i<npme; i++) sidx[i]=sdispls[i];
    for(i=0; (i<n); i++) {
      ii=sidx[idxa[i]];
      sidx[idxa[i]]++;
      buf[ii]=charge[i];
    }
    MPI_Alltoallv(buf, scounts, sdispls, mpi_type,
                  pme_q, rcounts, rdispls, mpi_type,
                  cr->mpi_comm_mygroup);

  }
  else { /* backward, redistribution from pme to pp */ 
    MPI_Alltoallv(pme_x, rcounts, rdispls, rvec_mpi,
                  buf, scounts, sdispls, rvec_mpi,
                  cr->mpi_comm_mygroup);

/* Copy data from receive buffer */
    for(i=0; i<npme; i++)
      sidx[i] = sdispls[i];
    for(i=0; (i<n); i++) {
      ii = DIM*sidx[idxa[i]];
      x_f[i][XX] = buf[ii+XX];
      x_f[i][YY] = buf[ii+YY];
      x_f[i][ZZ] = buf[ii+ZZ];
      sidx[idxa[i]]++;
    }
  }
#endif 
}

static void gmx_sum_qgrid_dd(gmx_pme_t pme,t_commrec *cr,t_fftgrid *grid,
			     int direction)
{
  static bool bFirst=TRUE;
  static real *tmp;
  int i;
  static int localsize;
  static int maxproc;
  int ny, la2r;
  int bndsize;
  real *from, *to;
#ifdef GMX_MPI
  MPI_Status stat;
#endif
  static int nodeshift=0;

  GMX_MPE_LOG(ev_sum_qgrid_start);
  
#ifdef GMX_MPI
  if(bFirst) {
    bFirst=FALSE;
    localsize=grid->la12r*grid->pfft.local_nx;
    if(!grid->workspace)
      snew(tmp,localsize);
    maxproc=grid->nx/grid->pfft.local_nx;
    /* Nodeshift: same meaning as in spread_q_bsplines */
    if (pmeduty(cr)==epmePMEONLY)
      nodeshift=cr->nnodes-cr->npmenodes;
  }
  /* NOTE: FFTW doesnt necessarily use all processors for the fft;
     * above I assume that the ones that do have equal amounts of data.
     * this is bad since its not guaranteed by fftw, but works for now...
     * This will be fixed in the next release.
     */
  if (grid->workspace)
    tmp=grid->workspace;
  if (direction  == GMX_SUM_QGRID_FORWARD) { 
    /* sum contributions to local grid */
    ny=grid->ny;
    la2r=grid->la2r;
/* Send left Boundary */
    bndsize = pme->leftbnd*ny*la2r;
    from = grid->ptr + (cr->left   - nodeshift + 1)*localsize - bndsize;
    to   = grid->ptr + (cr->nodeid - nodeshift + 1)*localsize - bndsize;
    MPI_Sendrecv(from,bndsize,mpi_type,
               cr->left,cr->nodeid,
               tmp,bndsize,mpi_type,cr->right,cr->right,
               cr->mpi_comm_mysim,&stat);
    GMX_MPE_LOG(ev_test_start); 
    for(i=0; (i<bndsize); i++) {
      to[i] += tmp[i];
    }
    GMX_MPE_LOG(ev_test_finish);

/* Send right boundary */
    bndsize = pme->rightbnd*ny*la2r;
    from = grid->ptr + (cr->right  - nodeshift)*localsize;
    to   = grid->ptr + (cr->nodeid - nodeshift)*localsize;
    MPI_Sendrecv(from,bndsize,mpi_type,
               cr->right,cr->nodeid,
               tmp,bndsize,mpi_type,cr->left,cr->left,
               cr->mpi_comm_mysim,&stat); 
    GMX_MPE_LOG(ev_test_start);
    for(i=0; (i<bndsize); i++) {
      to[i] += tmp[i];
    }
    GMX_MPE_LOG(ev_test_finish);
  }
  else if (direction  == GMX_SUM_QGRID_BACKWARD) { 
    /* distribute local grid to all processors */
    ny=grid->ny;
    la2r=grid->la2r;

/* Send left Boundary */
    bndsize = pme->rightbnd*ny*la2r;
    from = grid->ptr + (cr->nodeid - nodeshift)*localsize;
    to   = grid->ptr + (cr->right  - nodeshift)*localsize;
    MPI_Sendrecv(from,bndsize,mpi_type,
               cr->left,cr->nodeid,
               to,bndsize,mpi_type,cr->right,cr->right,
               cr->mpi_comm_mysim,&stat);
/* Send right boundary */
    bndsize = pme->leftbnd*ny*la2r;
    from = grid->ptr + (cr->nodeid - nodeshift + 1)*localsize - bndsize;
    to   = grid->ptr + (cr->left   - nodeshift + 1)*localsize - bndsize;
    MPI_Sendrecv(from,bndsize,mpi_type,
               cr->right,cr->nodeid,
               to,bndsize,mpi_type,cr->left,cr->left,
               cr->mpi_comm_mysim,&stat);
  }
  else
    gmx_fatal(FARGS,"Invalid direction %d for summing qgrid",direction);
    
#else
  gmx_fatal(FARGS,"Parallel grid summation requires MPI and FFTW.\n");    
#endif
  GMX_MPE_LOG(ev_sum_qgrid_finish);
}

void gmx_sum_qgrid(gmx_pme_t gmx,t_commrec *cr,t_fftgrid *grid,int direction)
{
  static bool bFirst=TRUE;
  static real *tmp;
  int i;
  static int localsize;
  static int maxproc;

#ifdef GMX_MPI
  if(bFirst) {
    localsize=grid->la12r*grid->pfft.local_nx;
    if(!grid->workspace)
      snew(tmp,localsize);
    maxproc=grid->nx/grid->pfft.local_nx;
  }
  /* NOTE: FFTW doesnt necessarily use all processors for the fft;
     * above I assume that the ones that do have equal amounts of data.
     * this is bad since its not guaranteed by fftw, but works for now...
     * This will be fixed in the next release.
     */
  bFirst=FALSE;
  if (grid->workspace)
    tmp=grid->workspace;
  if (direction == GMX_SUM_QGRID_FORWARD) { 
    /* sum contributions to local grid */

    GMX_BARRIER(cr->mpi_comm_mygroup);
    GMX_MPE_LOG(ev_reduce_start);
    for(i=0;i<maxproc;i++) {
      MPI_Reduce(grid->ptr+i*localsize, /*ptr arithm.     */
		 tmp,localsize,      
		 GMX_MPI_REAL,MPI_SUM,i,cr->mpi_comm_mygroup);
    }
    GMX_MPE_LOG(ev_reduce_finish);

    if(cr->nodeid<maxproc)
      memcpy(grid->ptr+cr->nodeid*localsize,tmp,localsize*sizeof(real));
  }
  else if (direction == GMX_SUM_QGRID_BACKWARD) { 
    /* distribute local grid to all processors */
    for(i=0;i<maxproc;i++)
      MPI_Bcast(grid->ptr+i*localsize, /* ptr arithm     */
		localsize,       
		GMX_MPI_REAL,i,cr->mpi_comm_mygroup);
  }
  else
    gmx_fatal(FARGS,"Invalid direction %d for summing qgrid",direction);
    
#else
  gmx_fatal(FARGS,"Parallel grid summation requires MPI and FFTW.\n");    
#endif
}

static void spread_q_bsplines(gmx_pme_t pme,t_fftgrid *grid,real charge[],
			      int nr,int order,t_commrec *cr)
{
  /* spread charges from home atoms to local grid */
  real     *ptr;
  int      i,nn,n,*i0,*j0,*k0,*ii0,*jj0,*kk0,ithx,ithy,ithz;
  int      nx,ny,nz,nx2,ny2,nz2,la2,la12;
  int      norder,*idxptr,index_x,index_xy,index_xyz;
  real     valx,valxy,qn;
  real     *thx,*thy,*thz;
  int localsize, bndsize;
  static bool bFirst=TRUE;
  static int nodeshift=0;

  if(bFirst) {
    bFirst=FALSE;
    /* Variable nodeshift accounts for correct calculation of ptr
       if the first PME node has a nodeid of >0. */
    if (pmeduty(cr)==epmePMEONLY)
      nodeshift = cr->nnodes-cr->npmenodes;
  }
  
  if (!pme->bPar) {
    clear_fftgrid(grid); 
#ifdef GMX_MPI
  } else {
    localsize = grid->la12r*grid->pfft.local_nx;
    ptr = grid->localptr;
    for (i=0; (i<localsize); i++)
      ptr[i] = 0;
/* clear left boundary area */
    bndsize = pme->leftbnd*grid->la12r;
    ptr = grid->ptr + (cr->left - nodeshift + 1)*localsize - bndsize;
    for (i=0; (i<bndsize); i++)
      ptr[i] = 0;
/* clear right boundary area */
    bndsize = pme->rightbnd*grid->la12r;
    ptr = grid->ptr + (cr->right - nodeshift)*localsize;
    for (i=0; (i<bndsize); i++)
      ptr[i] = 0;
#endif
  }
  unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
  ii0   = pme->nnx+nx2+1-order/2;
  jj0   = pme->nny+ny2+1-order/2;
  kk0   = pme->nnz+nz2+1-order/2;
  
  for(nn=0; (nn<nr); nn++) {
#ifdef SORTPME
    n = pme->pmeatom[nn].atom;
#else
    n = nn;
#endif
    qn     = charge[n];
    idxptr = pme->idx[n];
    
    if (qn != 0) {
      norder  = n*order;
      
      /* Pointer arithmetic alert, next six statements */
      i0  = ii0 + idxptr[XX]; 
      j0  = jj0 + idxptr[YY];
      k0  = kk0 + idxptr[ZZ];
      thx = pme->theta[XX] + norder;
      thy = pme->theta[YY] + norder;
      thz = pme->theta[ZZ] + norder;
      
      for(ithx=0; (ithx<order); ithx++) {
	index_x = la12*i0[ithx];
	valx    = qn*thx[ithx];
		
	for(ithy=0; (ithy<order); ithy++) {
	  valxy    = valx*thy[ithy];
	  index_xy = index_x+la2*j0[ithy];
	  
	  for(ithz=0; (ithz<order); ithz++) {
	    index_xyz       = index_xy+k0[ithz];
	    ptr[index_xyz] += valxy*thz[ithz];
	  }
	}
      }
    }
  }
}

real solve_pme(gmx_pme_t pme,t_fftgrid *grid,
	       real ewaldcoeff,real vol,matrix vir,t_commrec *cr)
{
  /* do recip sum over local cells in grid */
  t_complex *ptr,*p0;
  int     nx,ny,nz,nx2,ny2,nz2,la2,la12;
  int     kx,ky,kz,idx,idx0,maxkx,maxky,maxkz,kystart=0,kyend=0;
  real    m2,mx,my,mz;
  real    factor=M_PI*M_PI/(ewaldcoeff*ewaldcoeff);
  real    ets2,struct2,vfactor,ets2vf;
  real    eterm,d1,d2,energy=0;
  real    denom;
  real    bx,by;
  real    mhx,mhy,mhz;
  real    virxx=0,virxy=0,virxz=0,viryy=0,viryz=0,virzz=0;
  real    rxx,ryx,ryy,rzx,rzy,rzz;
  
  unpack_fftgrid(grid,&nx,&ny,&nz,
		 &nx2,&ny2,&nz2,&la2,&la12,FALSE,(real **)&ptr);
   
  rxx = pme->recipbox[XX][XX];
  ryx = pme->recipbox[YY][XX];
  ryy = pme->recipbox[YY][YY];
  rzx = pme->recipbox[ZZ][XX];
  rzy = pme->recipbox[ZZ][YY];
  rzz = pme->recipbox[ZZ][ZZ];
 
  maxkx = (nx+1)/2;
  maxky = (ny+1)/2;
  maxkz = nz/2+1;
    
  if (pme->bPar) { 
    /* transpose X & Y and only sum local cells */
#ifdef GMX_MPI
    kystart = grid->pfft.local_y_start_after_transpose;
    kyend   = kystart+grid->pfft.local_ny_after_transpose;
    if (debug)
      fprintf(debug,"solve_pme: kystart = %d, kyend=%d\n",kystart,kyend);
#else
    gmx_fatal(FARGS,"Parallel PME attempted without MPI and FFTW");
#endif /* end of parallel case loop */
  }
  else {
    kystart = 0;
    kyend   = ny;
  }
  
  for(ky=kystart; (ky<kyend); ky++) {  /* our local cells */
    
    if(ky<maxky)
      my = ky;
    else
      my = (ky-ny);
    by = M_PI*vol*pme->bsp_mod[YY][ky];
    
    for(kx=0; (kx<nx); kx++) {    
      if(kx < maxkx)
	mx = kx;
      else
	mx = (kx-nx);

      mhx = mx * rxx;
      mhy = mx * ryx + my * ryy;

      bx = pme->bsp_mod[XX][kx];
      
      if (pme->bPar)
	p0 = ptr + INDEX(ky,kx,0); /* Pointer Arithmetic */
      else
	p0 = ptr + INDEX(kx,ky,0); /* Pointer Arithmetic */

      for(kz=0; (kz<maxkz); kz++,p0++)  {
	if ((kx==0) && (ky==0) && (kz==0))
	  continue;
	d1      = p0->re;
	d2      = p0->im;
	mz      = kz;

	mhz = mx * rzx + my * rzy + mz * rzz;

	m2      = mhx*mhx+mhy*mhy+mhz*mhz;
	denom   = m2*bx*by*pme->bsp_mod[ZZ][kz];
	eterm   = ONE_4PI_EPS0*exp(-factor*m2)/(pme->epsilon_r*denom);
	p0->re  = d1*eterm;
	p0->im  = d2*eterm;
	
	struct2 = d1*d1+d2*d2;
	if ((kz > 0) && (kz < (nz+1)/2))
	  struct2*=2;
	ets2     = eterm*struct2;
	vfactor  = (factor*m2+1)*2.0/m2;
	energy  += ets2;
	
	ets2vf   = ets2*vfactor;
	virxx   += ets2vf*mhx*mhx-ets2;
	virxy   += ets2vf*mhx*mhy;   
	virxz   += ets2vf*mhx*mhz;  
	viryy   += ets2vf*mhy*mhy-ets2;
	viryz   += ets2vf*mhy*mhz;
	virzz   += ets2vf*mhz*mhz-ets2;
      }
    }
  }
    
  /* Update virial with local values. The virial is symmetric by definition.
   * this virial seems ok for isotropic scaling, but I'm
   * experiencing problems on semiisotropic membranes.
   * IS THAT COMMENT STILL VALID??? (DvdS, 2001/02/07).
   */
  vir[XX][XX] = 0.25*virxx;
  vir[YY][YY] = 0.25*viryy;
  vir[ZZ][ZZ] = 0.25*virzz;
  vir[XX][YY] = vir[YY][XX] = 0.25*virxy;
  vir[XX][ZZ] = vir[ZZ][XX] = 0.25*virxz;
  vir[YY][ZZ] = vir[ZZ][YY] = 0.25*viryz;
   
  /* This energy should be corrected for a charged system */
  return(0.5*energy);
}

void gather_f_bsplines(gmx_pme_t pme,t_fftgrid *grid,
		       rvec f[],real *charge,real scale)
{
  /* sum forces for local particles */  
  int     nn,n,*i0,*j0,*k0,*ii0,*jj0,*kk0,ithx,ithy,ithz;
  int     nx,ny,nz,nx2,ny2,nz2,la2,la12,index_x,index_xy;
  real *  ptr;
  real    tx,ty,dx,dy,qn;
  real    fx,fy,fz,fnx,fny,fnz,gval,tgz,dgz;
  real    gval1,gval2,gval3,gval4;
  real    fxy1,fz1;
  real    *thx,*thy,*thz,*dthx,*dthy,*dthz;
  int     sn,norder,*idxptr,ind0;
  real    rxx,ryx,ryy,rzx,rzy,rzz;
  int     order;
  
  unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
  order = pme->pme_order;
  thx   = pme->theta[XX];
  thy   = pme->theta[YY];
  thz   = pme->theta[ZZ];
  dthx  = pme->dtheta[XX];
  dthy  = pme->dtheta[YY];
  dthz  = pme->dtheta[ZZ];
  ii0   = pme->nnx+nx2+1-order/2;
  jj0   = pme->nny+ny2+1-order/2;
  kk0   = pme->nnz+nz2+1-order/2;
  
  rxx   = pme->recipbox[XX][XX];
  ryx   = pme->recipbox[YY][XX];
  ryy   = pme->recipbox[YY][YY];
  rzx   = pme->recipbox[ZZ][XX];
  rzy   = pme->recipbox[ZZ][YY];
  rzz   = pme->recipbox[ZZ][ZZ];

  for(nn=0; (nn<pme->my_homenr); nn++) {
#ifdef SORTPME
    n = pme->pmeatom[nn].atom;
#else
    n = nn;
#endif
    qn      = scale*charge[n];

    if (qn != 0) {
      fx     = 0.0;
      fy     = 0.0;
      fz     = 0.0;
      /*
      fnx    = f[n][XX];
      fny    = f[n][YY];
      fnz    = f[n][ZZ];
      */
      idxptr = pme->idx[n];
      norder = n*order;
      
      /* Pointer arithmetic alert, next nine statements */
      i0   = ii0 + idxptr[XX]; 
      j0   = jj0 + idxptr[YY];
      k0   = kk0 + idxptr[ZZ];
      thx  = pme->theta[XX] + norder;
      thy  = pme->theta[YY] + norder;
      thz  = pme->theta[ZZ] + norder;
      dthx = pme->dtheta[XX] + norder;
      dthy = pme->dtheta[YY] + norder;
      dthz = pme->dtheta[ZZ] + norder;
      
      for(ithx=0; (ithx<order); ithx++) {
	index_x = la12*i0[ithx];
        tx      = thx[ithx];
        dx      = dthx[ithx];

	for(ithy=0; (ithy<order); ithy++) {
	  index_xy = index_x+la2*j0[ithy];
	  ty       = thy[ithy];
	  dy       = dthy[ithy];
	  fxy1     = fz1 = 0;
	  
	  for(ithz=0; (ithz<order); ithz++) {
	    gval  = ptr[index_xy+k0[ithz]];
	    fxy1 += thz[ithz]*gval;
	    fz1  += dthz[ithz]*gval;
	  }
	  fx += dx*ty*fxy1;
	  fy += tx*dy*fxy1;
	  fz += tx*ty*fz1; 
	} 
      }
      /*
      f[n][XX] = fnx - qn*( fx*nx*rxx );
      f[n][YY] = fny - qn*( fx*nx*ryx + fy*ny*ryy );
      f[n][ZZ] = fnz - qn*( fx*nx*rzx + fy*ny*rzy + fz*nz*rzz );
      */
      f[n][XX] = -qn*( fx*nx*rxx );
      f[n][YY] = -qn*( fx*nx*ryx + fy*ny*ryy );
      f[n][ZZ] = -qn*( fx*nx*rzx + fy*ny*rzy + fz*nz*rzz );
    }
  }
  /* Since the energy and not forces are interpolated
   * the net force might not be exactly zero.
   * This can be solved by also interpolating F, but
   * that comes at a cost.
   * A better hack is to remove the net force every
   * step, but that must be done at a higher level
   * since this routine doesn't see all atoms if running
   * in parallel. Don't know how important it is?  EL 990726
   */
}


void make_bsplines(splinevec theta,splinevec dtheta,int order,int nx,int ny,
		   int nz,rvec fractx[],ivec idx[],real charge[],int nr)
{
  /* construct splines for local atoms */
  int  i,j,k,l;
  real dr,div,rcons;
  real *data,*ddata,*xptr;

  for(i=0; (i<nr); i++) {
    if (charge[i] != 0.0) {
      xptr = fractx[i];
      for(j=0; (j<DIM); j++) {
	dr  = xptr[j];
	
	/* dr is relative offset from lower cell limit */
	data=&(theta[j][i*order]);
	data[order-1]=0;
	data[1]=dr;
	data[0]=1-dr;
		
	for(k=3; (k<order); k++) {
	  div=1.0/(k-1.0);    
	  data[k-1]=div*dr*data[k-2];
	  for(l=1; (l<(k-1)); l++)
	    data[k-l-1]=div*((dr+l)*data[k-l-2]+(k-l-dr)*
			     data[k-l-1]);
	  data[0]=div*(1-dr)*data[0];
	}
	/* differentiate */
	ddata    = &(dtheta[j][i*order]);
	ddata[0] = -data[0];
	for(k=1; (k<order); k++)
	  ddata[k]=data[k-1]-data[k];
		
	div=1.0/(order-1);
	data[order-1]=div*dr*data[order-2];
	for(l=1; (l<(order-1)); l++)
	  data[order-l-1]=div*((dr+l)*data[order-l-2]+
			       (order-l-dr)*data[order-l-1]);
	data[0]=div*(1-dr)*data[0]; 
      }
    }
  }
}


void make_dft_mod(real *mod,real *data,int ndata)
{
  int i,j;
  real sc,ss,arg;
    
  for(i=0;i<ndata;i++) {
    sc=ss=0;
    for(j=0;j<ndata;j++) {
      arg=(2.0*M_PI*i*j)/ndata;
      sc+=data[j]*cos(arg);
      ss+=data[j]*sin(arg);
    }
    mod[i]=sc*sc+ss*ss;
  }
  for(i=0;i<ndata;i++)
    if(mod[i]<1e-7)
      mod[i]=(mod[i-1]+mod[i+1])*0.5;
}



void make_bspline_moduli(splinevec bsp_mod,int nx,int ny,int nz,int order)
{
  int nmax=max(nx,max(ny,nz));
  real *data,*ddata,*bsp_data;
  int i,k,l;
  real div;
    
  snew(data,order);
  snew(ddata,order);
  snew(bsp_data,nmax);

  data[order-1]=0;
  data[1]=0;
  data[0]=1;
	    
  for(k=3;k<order;k++) {
    div=1.0/(k-1.0);
    data[k-1]=0;
    for(l=1;l<(k-1);l++)
      data[k-l-1]=div*(l*data[k-l-2]+(k-l)*data[k-l-1]);
    data[0]=div*data[0];
  }
  /* differentiate */
  ddata[0]=-data[0];
  for(k=1;k<order;k++)
    ddata[k]=data[k-1]-data[k];
  div=1.0/(order-1);
  data[order-1]=0;
  for(l=1;l<(order-1);l++)
    data[order-l-1]=div*(l*data[order-l-2]+(order-l)*data[order-l-1]);
  data[0]=div*data[0]; 

  for(i=0;i<nmax;i++)
    bsp_data[i]=0;
  for(i=1;i<=order;i++)
    bsp_data[i]=data[i-1];
    
  make_dft_mod(bsp_mod[XX],bsp_data,nx);
  make_dft_mod(bsp_mod[YY],bsp_data,ny);
  make_dft_mod(bsp_mod[ZZ],bsp_data,nz);

  sfree(data);
  sfree(ddata);
  sfree(bsp_data);
}

int gmx_pme_destroy(FILE *log,gmx_pme_t *pmedata)
{
  fprintf(log,"Destroying PME data structures.\n");
  sfree((*pmedata)->nnx);
  sfree((*pmedata)->nny);
  sfree((*pmedata)->nnz);
  sfree((*pmedata)->idx);
  sfree((*pmedata)->fractx);
  sfree((*pmedata)->pmeatom);
  done_fftgrid((*pmedata)->gridA);
  if((*pmedata)->gridB)
    done_fftgrid((*pmedata)->gridB);
    
  sfree(*pmedata);
  *pmedata = NULL;
  
  return 0;
}

static void pme_realloc_homenr_things(gmx_pme_t pme,int homenr)
{
  int i;

  for(i=0;i<DIM;i++) {
    srenew(pme->theta[i],pme->pme_order*homenr); 
    srenew(pme->dtheta[i],pme->pme_order*homenr);
  }
  srenew(pme->fractx,homenr); 
  srenew(pme->pmeatom,homenr);
  srenew(pme->idx,homenr);
}

int gmx_pme_init(FILE *log,gmx_pme_t *pmedata,t_commrec *cr,
		 t_inputrec *ir,int homenr,
		 bool bFreeEnergy,bool bVerbose)
{
  gmx_pme_t pme=NULL;
  
  int i,npme; /* how many nodes participate in PME? */

  fprintf(log,"Creating PME data structures.\n");
  snew(pme,1);
  
  if (cr->npmenodes)
    npme = cr->npmenodes;
  else
    npme = cr->nnodes;
 
  fprintf(log,"Will do PME sum in reciprocal space.\n");
  please_cite(log,"Essman95a");

  if (ir->ewald_geometry == eewg3DC) {
    fprintf(log,"Using the Ewald3DC correction for systems with a slab geometry.\n");
    please_cite(log,"In-Chul99a");
  }

  pme->bFEP = ((ir->efep != efepNO) && bFreeEnergy);
  pme->nkx  = ir->nkx;
  pme->nky  = ir->nky;
  pme->nkz  = ir->nkz;
  pme->bPar = cr && (npme>1); 
  pme->pme_order   = ir->pme_order;
  pme->epsilon_r   = ir->epsilon_r;
  
  if (pme->bPar) {
    pme->leftbnd  = ir->pme_order/2 - 1;
    pme->rightbnd = ir->pme_order - pme->leftbnd - 1;

    fprintf(log,"Parallelized PME sum used. nkx=%d, npme=%d\n",ir->nkx,npme);
    if ((ir->nkx % npme) != 0)
      fprintf(log,"Warning: For load balance, "
	      "fourier_nx should be divisible by the number of PME nodes\n");
  } 

  /* With domain decomposition we need nnx on the PP only nodes */
  snew(pme->nnx,5*ir->nkx);
  snew(pme->nny,5*ir->nky);
  snew(pme->nnz,5*ir->nkz);
  for(i=0; (i<5*ir->nkx); i++)
    pme->nnx[i] = i % ir->nkx;
  for(i=0; (i<5*ir->nky); i++)
    pme->nny[i] = i % ir->nky;
  for(i=0; (i<5*ir->nkz); i++)
    pme->nnz[i] = i % ir->nkz;

  snew(pme->bsp_mod[XX],ir->nkx);
  snew(pme->bsp_mod[YY],ir->nky);
  snew(pme->bsp_mod[ZZ],ir->nkz);
  
  pme->gridA = mk_fftgrid(log,ir->nkx,ir->nky,ir->nkz,cr);
  if (bFreeEnergy)
    pme->gridB = mk_fftgrid(log,ir->nkx,ir->nky,ir->nkz,cr);
  else
    pme->gridB = NULL;
  
  make_bspline_moduli(pme->bsp_mod,ir->nkx,ir->nky,ir->nkz,ir->pme_order);
  
  if (!(pmeduty(cr)==epmePMEONLY && pme->bPar && DOMAINDECOMP(cr)))
    pme_realloc_homenr_things(pme,homenr);

  *pmedata = pme;
  
  return 0;
}

static void spread_on_grid(FILE *logfile,    gmx_pme_t pme,
			   t_fftgrid *grid,  t_commrec *cr,    
			   rvec x[],
			   real charge[],    matrix box,
			   bool bGatherOnly, bool bHaveSplines)
{ 
  int nx,ny,nz,nx2,ny2,nz2,la2,la12;
  real *ptr;
  
  /* Unpack structure */
  unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
  
  if (!bHaveSplines)
    /* Inverse box */
    calc_recipbox(box,pme->recipbox); 
  
  if (!bGatherOnly) {
    if (!bHaveSplines) {
      /* Compute fftgrid index for all atoms, with help of some extra variables */
      calc_idx(pme,x);
      
      /* make local bsplines  */
      make_bsplines(pme->theta,pme->dtheta,pme->pme_order,nx,ny,nz,
		    pme->fractx,pme->idx,charge,pme->my_homenr);
    }    

    /* put local atoms on grid. */
    spread_q_bsplines(pme,grid,charge,pme->my_homenr,pme->pme_order,cr);
/*    pr_grid_dist(logfile,"spread",grid); */
  }
}

/*************/
#ifdef GMX_MPI  
/*************/
/* The following routines are for split-off pme nodes */
static void pd_pme_send_x(t_nsborder *nsb, matrix box,
			  rvec x[], 
			  real chargeA[],  real chargeB[], 
			  bool bLastStep,  bool bFreeEnergy, 
			  t_commrec *cr)
{
  gmx_pme_comm_n_box_t cnb;
  int receiver, i;
  int firstpmenode=nsb->nnodes-nsb->npmenodes;
  int lastpmenode =nsb->nnodes-1;
  int ind_start,ind_end;
  static bool bFirst=TRUE;
  static int nsend;
  static MPI_Request req[3];
  static MPI_Status  stat[3];
  FILE *fp;
  char fn[8];

#ifdef PRT_COORDS
  if (bFirst) {
    sprintf(fn,"xsend%2.2d",cr->nodeid);
    fp=fopen(fn,"w");
    for (i=START(nsb); (i<START(nsb)+HOMENR(nsb)); i++) { 
      fprintf(fp,"x %5d  %20.11e %20.11e %20.11e\n",
              i,x[i][XX],x[i][YY],x[i][ZZ]);
      }
    fclose(fp);
  }
#endif

  nsend = 0;
  if (cr->nodeid < cr->npmenodes) {
    /* This communication could be avoided in most cases,
     * but for high performance one should anyhow use domain decomposition */
    cnb.natoms = 0;
    copy_mat(box,cnb.box);
    cnb.bLastStep = bLastStep;
    
    MPI_Isend(&cnb,sizeof(cnb),MPI_BYTE,
	      cr->nodeid+cr->nnodes-cr->npmenodes,0,cr->mpi_comm_mysim,
	      &req[nsend++]);
  }

  /* Communicate coordinates and charges to the PME nodes */
  /* find out which of our local data has to be sent do pme node i!
   * local data has indices from START(nsb) to START(nsb)+HOMENR(nsb)-1 
   */
  for (receiver=firstpmenode; receiver<=lastpmenode; receiver++) 
  {
    ind_start = max(START(nsb),nsb->pmeindex[receiver]);
    ind_end   = min(START(nsb)+HOMENR(nsb),
		    nsb->pmeindex[receiver]+nsb->pmehomenr[receiver]);
    if (ind_end > ind_start)
    {
/*    fprintf(stderr,"SEND: Node %d -> node %d (Indices %d to %d) - %f %f %f\n",
 *            nsb->nodeid,receiver,ind_start,ind_end,x[ind_start][0],x[ind_start][1],x[ind_start][2]);
 */
      if (MPI_Isend(&x[ind_start][0],(ind_end-ind_start)*DIM,
                    GMX_MPI_REAL,receiver,1,cr->mpi_comm_mysim,&req[nsend++]) !=0 )
        gmx_comm("MPI_Isend failed in send_coordinates");
      
      if (bFirst) /* we need to send the charges in the first step 
                   * (this can be done using blocking sends) */
      {
        MPI_Send(&chargeA[ind_start],(ind_end-ind_start),
	         GMX_MPI_REAL,receiver,2,cr->mpi_comm_mysim);
        if (bFreeEnergy) 
          MPI_Send(&chargeB[ind_start],(ind_end-ind_start),
	           GMX_MPI_REAL,receiver,3,cr->mpi_comm_mysim);
      }
    }
  }
  bFirst=FALSE;
  /* wait for the communication to finish! */
  MPI_Waitall(nsend, req, stat);
}

static void pd_pme_receive_f(t_commrec *cr, t_nsborder *nsb, 
			     rvec f[]     , int step)
{
  int  i,j,sender,elements;
  static int  firstpmenode, lastpmenode;
  int  ind_start, ind_end;
  MPI_Status  status;
  static rvec  *f_rec;  /* force receive buffer, don't lose address across calls! */
  static bool  bFirst=TRUE;
  MPI_Request req[2];
  MPI_Status  stat[2];
  int nrecv;
  double t0, t1;
  static double t2=0, t3;
  static double t_receive_f=0.0, tstep_sum=0.0;
  static int timesteps=0;

  if (bFirst)
  {
    firstpmenode=nsb->nnodes-nsb->npmenodes;
    lastpmenode =nsb->nnodes-1;
    snew(f_rec,HOMENR(nsb));
    bFirst=FALSE;
  }

#ifdef PRT_FORCELR
    static FILE *fp_f1;
    static char fn1[10];

    /* long-range forces from the PME nodes */
    sprintf(fn1,"f_recv%2.2d",cr->nodeid);
    fp_f1=(FILE *)fopen(fn1,"w");
#endif

  /* this is to help the user balance the ratio
   * of PME versus PP nodes */
  t3=MPI_Wtime()-t2;
  t2=MPI_Wtime();
  if (TAKETIME)
  {
    t0=MPI_Wtime();
    MPI_Barrier(cr->mpi_comm_mysim);
    t1=MPI_Wtime();
    t_receive_f += t1-t0;
    tstep_sum   += t3;
    timesteps++;
/*    fprintf(stderr," PP node %d, this time step %f, sum %f, steps %d, length %f, sum %f, ratio %f\n",
 *            cr->nodeid, t1-t0, t_receive_f, timesteps, t3, tstep_sum, t_receive_f/tstep_sum);
 */
    if (timesteps == 10)
      fprintf(stderr, "PP node %d waits %3.0f%% of the time in do_force.\n", cr->nodeid, 100*t_receive_f/tstep_sum);
  }

  /* now receive the forces */
  nrecv=0;
  for (sender=firstpmenode; sender<=lastpmenode; sender++)
  {
    ind_start=max(START(nsb), nsb->pmeindex[sender]);
    ind_end =min(START(nsb)+HOMENR(nsb), nsb->pmeindex[sender]+nsb->pmehomenr[sender]);
    if (ind_end > ind_start)
    {
      if (MPI_Irecv(f_rec[ind_start-START(nsb)],(ind_end-ind_start)*DIM,
                    GMX_MPI_REAL,sender,0,cr->mpi_comm_mysim,&req[nrecv]) != 0)
        gmx_comm("MPI_Irecv failed in receive_lrforces");
      nrecv++;
/*      fprintf(stderr,"   RECEIVE F: Node %d from node %d (Indices %d to %d) - %12.4e %12.4e %12.4e\n",
 *              nsb->nodeid,sender,ind_start,ind_end,
 *  	      f_rec[ind_start-START(nsb)][0],f_rec[ind_start-START(nsb)][1],f_rec[ind_start-START(nsb)][2]);
 */
    }
  }
  MPI_Waitall(nrecv, req, stat);

  /* add PME forces to f (that already contains corrections 
   * and must not be overridden at this point!) */
  for (i=START(nsb); i<START(nsb)+HOMENR(nsb); i++)
    rvec_inc(f[i], f_rec[i-START(nsb)]);
    
#ifdef PRT_FORCELR
  for(i=0; i<HOMENR(nsb); i++) 
  { 
    fprintf(fp_f1,"force %5d  %20.12e %20.12e %20.12e\n",
            i+START(nsb),f_rec[i][XX],f_rec[i][YY],f_rec[i][ZZ]);

  }
#endif

}

/* This routine is in domdec.c, should be called there */
static void index2xyz(ivec nc,int ind,ivec xyz)
{
  xyz[XX] = ind % nc[XX];
  xyz[YY] = (ind / nc[XX]) % nc[YY];
  xyz[ZZ] = ind / (nc[YY]*nc[XX]);
}

#define dd_index_rev(n,i) ((((i)[XX]*(n)[YY] + (i)[YY])*(n)[ZZ]) + (i)[ZZ])

static int dd_my_pmenode(t_commrec *cr)
{
  gmx_domdec_t *dd;
  ivec xyz;

  dd = cr->dd;

  index2xyz(dd->nc,dd->nodeid,xyz);
  
  return dd->nnodes + (int)(dd_index_rev(dd->nc,xyz)*cr->npmenodes/dd->nnodes);
}

static void get_my_ddnodes(gmx_domdec_t *dd,int npmenodes,int my_nodeid,
			   int *nmy_ddnodes,int **my_ddnodes)
{
  ivec xyz;
  int i,pmenodeid;

  snew(*my_ddnodes,(dd->nnodes+npmenodes-1)/npmenodes);

  *nmy_ddnodes = 0;
  for(i=0; i<dd->nnodes; i++) {
    index2xyz(dd->nc,i,xyz);
    
    pmenodeid =
      dd->nnodes + (int)(dd_index_rev(dd->nc,xyz)*npmenodes/dd->nnodes);
    if (pmenodeid == my_nodeid)
      (*my_ddnodes)[(*nmy_ddnodes)++] = i;
  }
}

static void dd_pme_send_x_q(t_commrec *cr, matrix box,
			    rvec x[], real chargeA[], real chargeB[],
			    bool bFreeEnergy, bool bLastStep)
{
  int  pme_node,n,nreq;
  gmx_pme_comm_n_box_t cnb;
  MPI_Request req[4];

  pme_node = dd_my_pmenode(cr);

  n = cr->dd->nat_local;
  cnb.natoms = n;
  copy_mat(box,cnb.box);
  cnb.bLastStep = bLastStep;

  nreq = 0;
  /* Communicate bLastTime (0/1) via the MPI tag */
  MPI_Isend(&cnb,sizeof(cnb),MPI_BYTE,
	    pme_node,0,cr->mpi_comm_mysim,&req[nreq++]);

  MPI_Isend(x[0],n*sizeof(rvec),MPI_BYTE,
	    pme_node,1,cr->mpi_comm_mysim,&req[nreq++]);
  MPI_Isend(chargeA,n*sizeof(real),MPI_BYTE,
	    pme_node,2,cr->mpi_comm_mysim,&req[nreq++]);
  if (bFreeEnergy)
    MPI_Isend(chargeB,n*sizeof(real),MPI_BYTE,
	      pme_node,3,cr->mpi_comm_mysim,&req[nreq++]);

  /* We would like to postpone this wait until dd_pme_receive_f,
   * but currently the coordinates can be (un)shifted.
   * We will have to wait with this until we have separate
   * arrays for the shifted and unshifted coordinates.
   */
  MPI_Waitall(nreq,req,MPI_STATUSES_IGNORE);
}

static void dd_pme_receive_f(t_commrec *cr,rvec f[])
{
  static rvec *f_rec=NULL;
  static int  nalloc=0;
  int natoms,i;

  natoms = cr->dd->nat_local;

  if (natoms > nalloc) {
    nalloc = over_alloc(natoms);
    srenew(f_rec,nalloc);
  }
  
  MPI_Recv(f_rec[0],natoms*sizeof(rvec),MPI_BYTE,
	   dd_my_pmenode(cr),0,cr->mpi_comm_mysim,
	   MPI_STATUS_IGNORE);

  for(i=0; i<natoms; i++)
    rvec_inc(f[i],f_rec[i]);
}

static void receive_virial_energy(t_commrec *cr,
				  matrix vir,real *energy) 
{
  int  sender;
  gmx_pme_comm_vir_ene_t cve;
  MPI_Status status;

  /* receive energy, virial */
  sender = cr->nodeid + (cr->nnodes - cr->npmenodes);
  if (sender < cr->nnodes) 
  {
    /* receive virial and energy */
    MPI_Recv(&cve,sizeof(cve),MPI_BYTE,sender,1,cr->mpi_comm_mysim,&status);
	
    m_add(vir,cve.vir,vir);
    *energy = cve.energy;
    
    bGotTermSignal = (cve.flags & PME_TERM);
    bGotUsr1Signal = (cve.flags & PME_USR1);
  } else {
    *energy = 0;
  }
}

void gmx_pme_send_x(t_commrec *cr, t_nsborder *nsb,
		    matrix box,rvec x[], 
		    real chargeA[],  real chargeB[], 
		    bool bFreeEnergy, bool bLastStep)
{
  if (DOMAINDECOMP(cr)) {
    dd_pme_send_x_q(cr,box,x,chargeA,chargeB,bFreeEnergy,bLastStep);
  } else {
    pd_pme_send_x(nsb,box,x,chargeA,chargeB,bLastStep,bFreeEnergy,cr);
  }
}

void gmx_pme_receive_f(t_commrec *cr, t_nsborder *nsb, 
		       rvec f[]     , matrix vir, 
		       real *energy, int step)
{
  if (DOMAINDECOMP(cr)) {
    dd_pme_receive_f(cr,f);
  } else {
    pd_pme_receive_f(cr,nsb,f,step);
  }
  
  receive_virial_energy(cr,vir,energy);
}

int gmx_pmeonly(FILE *logfile,   gmx_pme_t pme,
		t_commrec *cr,
		t_nsborder *nsb, t_nrnb *mynrnb,
		real ewaldcoeff, real lambda,
		bool bGatherOnly)
     
{
  MPI_Status  status;
  rvec  *xpme=NULL,*fpme=NULL;
  real  *chargeA=NULL,*chargeB=NULL,*chargepme;
  int  nppnodes,sender,receiver,natreceivedpp=0,elements,tag;
  int  nmy_ddnodes,*my_ddnodes;
  int  ind_start, ind_end;
  gmx_pme_comm_n_box_t *cnb;
  int  nalloc_natpp=0,nalloc_homenr=0;
  int  nx,ny,nz,nx2,ny2,nz2,la12,la2;
  int  i,j,q,ntot,npme;
  real *ptr;
  t_fftgrid  *grid;
  int  *pidx=NULL;
  int  *gidx=NULL; /* Grid index of particle, used for PME redist */
  rvec  *x_tmp=NULL;
  rvec  *f_tmp=NULL;
  real  *q_tmp=NULL;
  FILE  *fp_f3,*fp_f4;
  char  fn[8];
  real  energy_AB[2] = {0,0};
  matrix  vir_AB[2];
  gmx_pme_comm_vir_ene_t cve;
  real  buf[DIM*DIM+1];
  bool  bFirst=TRUE;
  real  vol;
  real  *dvdlambda=0;
  int messages; /* count the Isends or Ireceives */    
  MPI_Request *req;
  MPI_Status  *stat;
  bool bDone=FALSE;
  int count=0;
  double t0, t1, t2=0, t3, t_send_f=0.0, tstep_sum=0.0;
  bool bNS;
  unsigned long int step=0, timesteps=0;

#define ME (nsb->nodeid) 

  nppnodes = cr->nnodes - cr->npmenodes;

  /* Allocate space for the request handles, a lonely PME node may
     receive (and later send back) as many messages as there are PP nodes */
  snew(req ,3*nppnodes);
  snew(stat,3*nppnodes);

  if (!DOMAINDECOMP(cr)) {
    snew(xpme,PMEHOMENR(nsb));
    snew(fpme,PMEHOMENR(nsb));
    snew(chargeA,PMEHOMENR(nsb));
    if (pme->bFEP)
      snew(chargeB,PMEHOMENR(nsb));
    snew(cnb,1);
  } else {
    /* Determine the DD nodes we need to talk with */
    get_my_ddnodes(cr->dd,cr->npmenodes,cr->nodeid,&nmy_ddnodes,&my_ddnodes);
    snew(cnb,nmy_ddnodes);
  }
  if (pme->bPar) {
    /* With domain decompostion these arrays should be dynamic */
    snew(f_tmp,nsb->natoms);
    snew(x_tmp,nsb->natoms);
    snew(q_tmp,nsb->natoms);
    snew(pidx,pme->nkx);
    snew(gidx,nsb->natoms);
  } 

  MPI_Type_contiguous(DIM, mpi_type, &rvec_mpi);
  MPI_Type_commit(&rvec_mpi);

  
#ifdef PRT_FORCE
  sprintf(fn,"f_pme%2.2d",cr->nodeid);
  fp_f3=(FILE *)fopen(fn,"w");
#endif

  do /****** this is a quasi-loop over time steps! */
  {  
    /* I am a PME node, I need the data with indices starting from                
     * pmeindex[nsb->nodeid] up to pmeindex[nsb->nodeid]+pmehomenr[nsb->nodeid]-1
     * Find out from which ppnodes I have to receive this data.
     */
    messages=0;
    if (!DOMAINDECOMP(cr)) {
      /* Receive the box */
      MPI_Irecv(cnb,sizeof(cnb[0]),MPI_BYTE,
		cr->nodeid-nppnodes,0,cr->mpi_comm_mysim,&req[messages++]);
    for (sender=0; sender<nppnodes; sender++) /* index of sender pp node */
    {
      /* First data index to be received from PP node i: */
      ind_start=max(nsb->index[sender], nsb->pmeindex[ME]); 
      /* Last data index to be received from PP node i: */
      ind_end =min(nsb->index[sender]+nsb->homenr[sender], nsb->pmeindex[ME]+nsb->pmehomenr[ME]);
      if (ind_end > ind_start)
      {
        if (MPI_Irecv(&xpme[ind_start-nsb->pmeindex[ME]][0],
	              (ind_end-ind_start)*DIM,GMX_MPI_REAL,sender,
                      1,cr->mpi_comm_mysim,&req[messages++]) != 0)
          gmx_comm("MPI_Irecv failed in do_pmeonly");
		       
	if (bFirst) /* receive charges at first time-step */
	{
          MPI_Recv(&chargeA[ind_start-nsb->pmeindex[ME]],
	           (ind_end-ind_start),GMX_MPI_REAL,sender,
                   2,cr->mpi_comm_mysim,&status);
          if (pme->bFEP)
            MPI_Recv(&chargeB[ind_start-nsb->pmeindex[ME]],
	             (ind_end-ind_start),GMX_MPI_REAL,sender,
                     3,cr->mpi_comm_mysim,&status);		      
        }
      }
    }
    /* now we got the data! */
    bFirst=FALSE;
    MPI_Waitall(messages, req, stat);
    bDone = cnb[0].bLastStep;
    } else {
      /* Domain decomposition */
      /* Receive the send counts and box */
      for(sender=0; sender<nmy_ddnodes; sender++) {
	MPI_Irecv(cnb+sender,sizeof(cnb[0]),MPI_BYTE,
		  my_ddnodes[sender],0,
		  cr->mpi_comm_mysim,&req[messages++]);
      }
      MPI_Waitall(messages, req, stat);
      messages = 0;
      bDone = cnb[0].bLastStep;

      natreceivedpp = 0;
      for(sender=0; sender<nmy_ddnodes; sender++) {
	natreceivedpp += cnb[sender].natoms;
      }
      if (natreceivedpp > nalloc_natpp) {
	nalloc_natpp = over_alloc(natreceivedpp);

	srenew(xpme,nalloc_natpp);
	srenew(chargeA,nalloc_natpp);
	if (pme->bFEP)
	  srenew(chargeB,nalloc_natpp);
	srenew(fpme,nalloc_natpp);
      }

      /* Receive the coordinates in place */
      i = 0;
      for(sender=0; sender<nmy_ddnodes; sender++) {
	if (cnb[sender].natoms > 0) {
	  MPI_Irecv(xpme[i],cnb[sender].natoms*sizeof(rvec),MPI_BYTE,
		    my_ddnodes[sender],1,
		    cr->mpi_comm_mysim,&req[messages++]);
	  i += cnb[sender].natoms;
	}
      }

      /* Receive the charges in place */
      for(q=0; q<(pme->bFEP ? 2 : 1); q++) {
	if (q == 0)
	  chargepme = chargeA;
	else
	  chargepme = chargeB;
	i = 0;
	for(sender=0; sender<nmy_ddnodes; sender++) {
	  if (cnb[sender].natoms > 0) {
	    MPI_Irecv(&chargepme[i],cnb[sender].natoms*sizeof(real),MPI_BYTE,
		      my_ddnodes[sender],2+q,
		      cr->mpi_comm_mysim,&req[messages++]);
	    i += cnb[sender].natoms;
	  }
	}
      }

      /* Wait for the coordinates and charges to arrive */
      MPI_Waitall(messages, req, stat);
      messages = 0;
    }

    for(q=0; q<(pme->bFEP ? 2 : 1); q++) /* loop over q */
    {
      if (q == 0) {
        grid = pme->gridA;
	chargepme = chargeA;
      } else {
        grid = pme->gridB;
        chargepme = chargeB;
      }

      /* Unpack structure */
      unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
    
#ifdef PRT_FORCE_X
      if (count==0) {
        for(i=0; i<PMEHOMENR(nsb); i++) { 
          fprintf(fp_f4,"x %5d  %20.11e %20.11e %20.11e\n",
                 (i+PMESTART(nsb)),xpme[i][XX],xpme[i][YY],xpme[i][ZZ]);
	}       
      }
#endif
 
      if (cr->npmenodes == 1) {
	pme->my_homenr = nsb->natoms;
        x_tmp = xpme;
        f_tmp = fpme;
        q_tmp = chargepme;
      } else {
	if (!DOMAINDECOMP(cr))
	  natreceivedpp =  PMEHOMENR(nsb);
        GMX_MPE_LOG(ev_pmeredist_start);
        pme_calc_pidx(cr,natreceivedpp,cnb[0].box,xpme,
		      pme->nkx,pme->nnx,pidx,gidx,NULL);
	pme->my_homenr = nsb->natoms;
        pmeredist(cr,TRUE,natreceivedpp, xpme, chargepme, gidx, 
                  &pme->my_homenr, x_tmp, q_tmp);
        GMX_MPE_LOG(ev_pmeredist_finish);
	
	if (pme->my_homenr > nalloc_homenr) {
	  nalloc_homenr = over_alloc(pme->my_homenr);
	  pme_realloc_homenr_things(pme,nalloc_homenr);
	}
      }
      /* fprintf(logfile,"Node= %6d, pme local particles=%6d\n", cr->nodeid,my_homenr); */

      /* Spread the charges on a grid */
      GMX_MPE_LOG(ev_spread_on_grid_start);
      spread_on_grid(logfile,pme,grid,cr,
  		     x_tmp,q_tmp,cnb[0].box,bGatherOnly,
		     q==0 ? FALSE : TRUE); 
      GMX_MPE_LOG(ev_spread_on_grid_finish);

      if (!bGatherOnly) 
      {
        inc_nrnb(mynrnb,eNR_SPREADQBSP,
	         pme->pme_order*pme->pme_order*pme->pme_order*pme->my_homenr);
      
        /* sum contributions to local grid from other nodes */
        if (pme->bPar) 
 	  gmx_sum_qgrid_dd(pme,cr,grid,GMX_SUM_QGRID_FORWARD);
        if (debug)
	  pr_fftgrid(debug,"qgrid",grid);

        /* do 3d-fft */ 
        GMX_MPE_LOG(ev_gmxfft3d_start);
        gmxfft3D(grid,GMX_FFT_REAL_TO_COMPLEX,cr);
        GMX_MPE_LOG(ev_gmxfft3d_finish);

        /* solve in k-space for our local cells */
        vol = det(cnb[0].box);
        GMX_MPE_LOG(ev_solve_pme_start);
/*      energy_AB[q]=solve_pme(grid,ewaldcoeff,vol,bsp_mod,recipbox,
	  		       vir_AB[q],cr); */			       
        energy_AB[q]=solve_pme(pme,grid,ewaldcoeff,vol,vir_AB[q],cr);
        GMX_MPE_LOG(ev_solve_pme_finish);

        inc_nrnb(mynrnb,eNR_SOLVEPME,nx*ny*nz*0.5);

        /* do 3d-invfft */
        GMX_MPE_LOG(ev_gmxfft3d_start);
        gmxfft3D(grid,GMX_FFT_COMPLEX_TO_REAL,cr);
        GMX_MPE_LOG(ev_gmxfft3d_finish);

        /* distribute local grid to all nodes */
        if (pme->bPar) 
	  gmx_sum_qgrid_dd(pme,cr,grid,GMX_SUM_QGRID_BACKWARD); 
        if (debug)
	  pr_fftgrid(debug,"potential",grid);
	
        ntot  = grid->nxyz;  
        npme  = ntot*log((real)ntot)/(cr->npmenodes*log(2.0));
        inc_nrnb(mynrnb,eNR_FFT,2*npme);
      }
    
      /* interpolate forces for our local atoms */
      GMX_MPE_LOG(ev_gather_f_bsplines_start);
      gather_f_bsplines(pme,grid,f_tmp,q_tmp,
  		        pme->bFEP ? (q==0 ? 1.0-lambda : lambda) : 1.0);
      GMX_MPE_LOG(ev_gather_f_bsplines_finish);

      /* redistribute forces */
      if (pme->bPar) 
      {
        GMX_MPE_LOG(ev_pmeredist_start);
        pmeredist(cr,FALSE,natreceivedpp, fpme, chargepme, gidx, 
                  &pme->my_homenr, f_tmp, q_tmp);
        GMX_MPE_LOG(ev_pmeredist_finish);
      }

#ifdef PRT_FORCE
      if (count==10) {
      	fprintf(stderr, "### Printing forces in do_pmeonly ###\n");

        for(i=0; i<PMEHOMENR(nsb); i++) 
	{ 
          fprintf(fp_f3,"force %5d  %20.12e %20.12e %20.12e\n",
                 (i+PMESTART(nsb)),fpme[i][XX],fpme[i][YY],fpme[i][ZZ]);
        }	
      }
#endif
  
      inc_nrnb(mynrnb,eNR_GATHERFBSP,
  	       pme->pme_order*pme->pme_order*pme->pme_order*pme->my_homenr);
    } /* of q-loop */


    if (!pme->bFEP) {
      cve.energy = energy_AB[0];
      copy_mat(vir_AB[0],cve.vir);
    } else 
    {
      cve.energy = (1.0-lambda)*energy_AB[0] + lambda*energy_AB[1];
      *dvdlambda += energy_AB[1] - energy_AB[0];
      for(i=0; i<DIM; i++)
        for(j=0; j<DIM; j++)
	  cve.vir[i][j] =
	    (1.0-lambda)*vir_AB[0][i][j] + lambda*vir_AB[1][i][j];
    }
    if (debug)
      fprintf(debug,"PME mesh energy: %g\n",cve.energy);


    /* this is to help the user balance the ratio
     * of PME versus PP nodes */

    /* Determine whether or not Neighbour Searching is done on PP nodes 
     * bNS = ((ir->nstlist && (step % ir->nstlist==0))
     *        || step==0);
     */
    t3=MPI_Wtime()-t2;
    t2=MPI_Wtime();
    if (TAKETIME)
    {
      t0=MPI_Wtime();
      MPI_Barrier(cr->mpi_comm_mysim);
      t1=MPI_Wtime();
      t_send_f += t1-t0;
      tstep_sum   += t3;
      timesteps++;
/*      fprintf(stderr," PME node %d, this time step %f, sum %f, steps %d, length t3=%f, tstep_sum=%f, ratio %f\n",
 *              cr->nodeid, t1-t0, t_send_f, timesteps, t3, tstep_sum, t_send_f/tstep_sum);
 */
      if (timesteps % 10 ==0)
        fprintf(stderr, "PME node %d waits %3.0f%% of the time step.\n", cr->nodeid, 100*t_send_f/tstep_sum);
    }

    /* Now the evaluated forces have to be transferred to the PP nodes */
    messages = 0;
    if (!DOMAINDECOMP(cr)) {
      for (receiver=0; receiver < nsb->nnodes-nsb->npmenodes; receiver++) {
	ind_start = max(nsb->index[receiver], nsb->pmeindex[ME]);
	ind_end   = min(nsb->index[receiver]+nsb->homenr[receiver],
			nsb->pmeindex[ME]+nsb->pmehomenr[ME]);
	if (ind_end > ind_start) {
	  if (MPI_Isend(fpme[ind_start-nsb->pmeindex[ME]],
			(ind_end-ind_start)*DIM,GMX_MPI_REAL,receiver,0,
			cr->mpi_comm_mysim,&req[messages++]) != 0)
	    gmx_comm("MPI_Isend failed in do_pmeonly");
	}
      }
    } else {
      ind_end = 0;
      for (receiver=0; receiver<nmy_ddnodes; receiver++) {
	ind_start = ind_end;
	ind_end   = ind_start + cnb[receiver].natoms;
	if (MPI_Isend(fpme[ind_start-nsb->pmeindex[ME]],
	              (ind_end-ind_start)*DIM,GMX_MPI_REAL,
		      my_ddnodes[receiver],0,
		      cr->mpi_comm_mysim,&req[messages++]) != 0)
	  gmx_comm("MPI_Isend failed in do_pmeonly");
      }
    }

    /* send energy, virial */
    /* task i out of the PME nodes group sends to task i out of the PP nodes group.
     * since we normally have more PP nodes than PME nodes some of the PP nodes will 
     * get no values. This is no problem since all these values are summed anyway in 
     * global_stat 
     */
    receiver = cr->nodeid - (cr->nnodes - cr->npmenodes);

    /* send virial and energy */
    cve.flags    = 0;
    if (bGotTermSignal)
      cve.flags |= PME_TERM;
    if (bGotUsr1Signal)
      cve.flags |= PME_USR1;

    MPI_Isend(&cve,sizeof(cve),MPI_BYTE,
	      receiver,1,cr->mpi_comm_mysim,&req[messages++]);

    /* Wait for the forces to arrive */
    MPI_Waitall(messages, req, stat);

    count++;

    /* These variables are needed clean in the next time step: 
     *
     * zero out f_tmp vector, since gather_f_bsplines
     * only subsequently adds to f_tmp:
     */
    //clear_rvecs(pme->my_homenr,f_tmp);
    
    /* Keep track of time step */
    step++;
    //MPI_Barrier(cr->mpi_comm_mysim); /* 100 */
  } /***** end of quasi-loop */
  while (!bDone);
     
#undef ME
  return 0;
}

/******************/
#endif /* GMX_MPI */  
/******************/


int gmx_pme_do(FILE *logfile,   gmx_pme_t pme,
	       rvec x[],        rvec f[],
	       real *chargeA,   real *chargeB,
	       matrix box,	t_commrec *cr,
	       t_nsborder *nsb, t_nrnb *nrnb,    
	       matrix vir,      real ewaldcoeff,
	       real *energy,    real lambda, 
	       real *dvdlambda, bool bGatherOnly)
{ 
  static  real energy_AB[2] = {0,0};
  int     q,i,j,ntot,npme;
  int     nx,ny,nz,nx2,ny2,nz2,la12,la2;
  t_fftgrid *grid=NULL;
  real *ptr;
  real    *homecharge=NULL,vol;
  matrix  vir_AB[2];
  static bool bFirst=TRUE;
  static int *pidx;
  static int *gidx=NULL; /* Grid index of particle, used for PME redist */
  static rvec *x_tmp;
  static rvec *f_tmp;
  static real *q_tmp;
  static FILE *fp_f, *fp_f4;
  static char fn[11];
  static int count=0;   
  
  
  if(bFirst) {
#if defined GMX_MPI
    if (pme->bPar) {
      snew(gidx,nsb->natoms);
      snew(x_tmp,nsb->natoms);
      snew(f_tmp,nsb->natoms);
      snew(q_tmp,nsb->natoms);
      snew(pidx,pme->nkx);
      MPI_Type_contiguous(DIM, mpi_type, &rvec_mpi);
      MPI_Type_commit(&rvec_mpi);
    }
#endif

#ifdef PRT_FORCE
    sprintf(fn,"f_do_pme%2.2d",cr->nodeid);
    fp_f=(FILE *)fopen(fn,"w");
#endif
    where();
    bFirst=FALSE;
  }
  for(q=0; q<(pme->bFEP ? 2 : 1); q++) {
    if (q == 0) {
      grid = pme->gridA;
      homecharge = chargeA+START(nsb);
    } else {
      grid = pme->gridB;
      homecharge = chargeB+START(nsb);
    }
    /* Unpack structure */
    if (debug) {
      fprintf(debug,"PME: nnodes = %d, nodeid = %d\n",cr->nnodes,cr->nodeid);
      fprintf(debug,"Grid = %p\n",grid);
      if (grid == NULL)
	gmx_fatal(FARGS,"No grid!");
    }
    where();
    unpack_fftgrid(grid,&nx,&ny,&nz,&nx2,&ny2,&nz2,&la2,&la12,TRUE,&ptr);
    where();

    pme->my_homenr = nsb->natoms;

#ifdef PRT_FORCE_X
    if (count==0)
      for (i=START(nsb); (i<START(nsb)+HOMENR(nsb)); i++) { 
        fprintf(fp_f4,"x %5d  %20.11e %20.11e %20.11e\n",
                i,x[i][XX],x[i][YY],x[i][ZZ]);
      }
#endif

    if (!pme->bPar) {
      x_tmp=x;
      f_tmp=f;
      q_tmp=homecharge;
    } else {
      pme_calc_pidx(cr,HOMENR(nsb),box,x+START(nsb),
		    pme->nkx,pme->nnx,pidx,gidx,NULL);
      where();

      GMX_BARRIER(cr->mpi_comm_mysim);
      pmeredist(cr, TRUE, HOMENR(nsb), x+START(nsb), homecharge, gidx, 
		&pme->my_homenr, x_tmp, q_tmp);
      where();
    }
    where();
    if (debug)
      fprintf(debug,"Node= %6d, pme local particles=%6d\n",
	      cr->nodeid,pme->my_homenr);

    /* Spread the charges on a grid */
    GMX_MPE_LOG(ev_spread_on_grid_start);

    /* Spread the charges on a grid */
    spread_on_grid(logfile,pme,grid,cr,
		   x_tmp,q_tmp,box,bGatherOnly,
		   q==0 ? FALSE : TRUE);
    GMX_MPE_LOG(ev_spread_on_grid_finish);

    if (!bGatherOnly) {
      inc_nrnb(nrnb,eNR_SPREADQBSP,
	       pme->pme_order*pme->pme_order*pme->pme_order*pme->my_homenr);
      
      /* sum contributions to local grid from other nodes */
      if (pme->bPar) {
        GMX_BARRIER(cr->mpi_comm_mysim);
	gmx_sum_qgrid_dd(pme,cr,grid,GMX_SUM_QGRID_FORWARD);
	where();
      }
#ifdef DEBUG
      if (debug)
	pr_fftgrid(debug,"qgrid",grid);
#endif
      where();

      /* do 3d-fft */ 
      GMX_BARRIER(cr->mpi_comm_mysim);
      GMX_MPE_LOG(ev_gmxfft3d_start);
      gmxfft3D(grid,GMX_FFT_REAL_TO_COMPLEX,cr);
      GMX_MPE_LOG(ev_gmxfft3d_finish);

      where();

      /* solve in k-space for our local cells */
      vol = det(box);
      GMX_BARRIER(cr->mpi_comm_mysim);
      GMX_MPE_LOG(ev_solve_pme_start);
      energy_AB[q]=solve_pme(pme,grid,ewaldcoeff,vol,vir_AB[q],cr);
      where();
      GMX_MPE_LOG(ev_solve_pme_finish);
      inc_nrnb(nrnb,eNR_SOLVEPME,nx*ny*nz*0.5);

      /* do 3d-invfft */
      GMX_BARRIER(cr->mpi_comm_mysim);
      GMX_MPE_LOG(ev_gmxfft3d_start);
      where();
      gmxfft3D(grid,GMX_FFT_COMPLEX_TO_REAL,cr);
      where();
      GMX_MPE_LOG(ev_gmxfft3d_finish);
      
      /* distribute local grid to all nodes */
      if (pme->bPar) {
        GMX_BARRIER(cr->mpi_comm_mysim);
	gmx_sum_qgrid_dd(pme,cr,grid,GMX_SUM_QGRID_BACKWARD);
      }
      where();
#ifdef DEBUG
      if (debug)
	pr_fftgrid(debug,"potential",grid);
#endif

      ntot  = grid->nxyz;  
      npme  = ntot*log((real)ntot)/log(2.0);
      if (pme->bPar)
	npme /= (cr->nnodes - cr->npmenodes);
      inc_nrnb(nrnb,eNR_FFT,2*npme);
      where();
    }
    /* interpolate forces for our local atoms */
    GMX_BARRIER(cr->mpi_comm_mysim);
    GMX_MPE_LOG(ev_gather_f_bsplines_start);
    
    if (pme->bPar) {
      for(i=0; (i<pme->my_homenr); i++) {
	f_tmp[i][XX]=0;
	f_tmp[i][YY]=0;
	f_tmp[i][ZZ]=0;
      }
    }
    where();
    gather_f_bsplines(pme,grid,f_tmp,q_tmp,
		      pme->bFEP ? (q==0 ? 1.0-lambda : lambda) : 1.0);
    where();
    
    GMX_MPE_LOG(ev_gather_f_bsplines_finish);
    
    if (pme->bPar) {
      GMX_BARRIER(cr->mpi_comm_mysim);
      pmeredist(cr, FALSE,HOMENR(nsb), f+START(nsb), homecharge, gidx, 
		&pme->my_homenr, f_tmp, q_tmp);
    }
    where();
    
#ifdef PRT_FORCE
    if (count==10) 
    {
      fprintf(stderr, "*** Printing forces in do_pme ***\n");  

      for(i=START(nsb); (i<START(nsb)+HOMENR(nsb)); i++) { 
/*      fprintf(fp_f,"force %5d  %20.12e %20.12e %20.12e %20.12e\n",
                      i,f[i][XX],f[i][YY],f[i][ZZ],homecharge[i-START(nsb)]); */
        fprintf(fp_f,"force %5d  %20.12e %20.12e %20.12e\n",
                i,f[i][XX],f[i][YY],f[i][ZZ]);
      }
    }
#endif
    count++;
    inc_nrnb(nrnb,eNR_GATHERFBSP,
	     pme->pme_order*pme->pme_order*pme->pme_order*pme->my_homenr);
  } /* of q-loop */
  
  if (!pme->bFEP) {
    *energy = energy_AB[0];
    copy_mat(vir_AB[0],vir);
  } else {
    *energy = (1.0-lambda)*energy_AB[0] + lambda*energy_AB[1];
    *dvdlambda += energy_AB[1] - energy_AB[0];
    for(i=0; i<DIM; i++)
      for(j=0; j<DIM; j++)
	vir[i][j] = (1.0-lambda)*vir_AB[0][i][j] + lambda*vir_AB[1][i][j];
  }
  if (debug)
    fprintf(debug,"PME mesh energy: %g\n",*energy);

  return 0;
}
