//
// mri_ms_EM.c
//
// original author: Xiao Han
// Implementation based on: H. Ichihashi et al. "Gaussian Mixture PDF 
//  approximation and fuzzy c-means clustering with Entropy Regularization  
//  Things to do:
//  1. use correct Gauss-Seidel iteration when performing ICM iteration of MRF
//  2. Is it more robust if I ignore correlation terms in covariance matrix?
//  3. Should I incorporae PVE model as mmfast?
// 1-31-05: added regularization for covariance matrix according to
// C. Archambeau et al Flexible and Robust Bayesian Classification by Finite Mixture Models, ESANN'2004
// Warning: Do not edit the following four lines.  CVS maintains them.
// Revision Author: $Author: xhan $
// Revision Date  : $Date: 2005/02/14 20:21:32 $
// Revision       : $Revision: 1.3 $
//
////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "nr.h"
#include "nrutil.h"
#include "mri.h"
#include "matrix.h"
#include "macros.h"
#include "error.h"
#include "diag.h"
#include "proto.h"
#include "mrimorph.h"
#include "mri_conform.h"
#include "utils.h"
#include "timer.h"
#include "matrix.h"
#include "transform.h"
#include "version.h"
#include "label.h"
#include "mrinorm.h"

#define SWAP(a,b) itemp=(a);(a)=(b);(b)=itemp;


/* maximum number of classes */
#define MAX_CLASSES 20
#define MEMTHRESHOLD 0.7 

static int xoff[6] = {1, -1, 0, 0, 0, 0};
static int yoff[6] = {0,  0, 1, -1, 0, 0 };
static int zoff[6] = {0,  0, 0,  0, 1, -1};

/* If set at 0.7, then Mahalanobis distance is 26.x; if set to 0.5, it's only 14.6. Of cause, lower threshold includes more partial volumed voxels, and increasecovariance */
/* For fair comparison, need to generate a hard segmentation! */

static int fix_class_size = 1;
static double kappa = 0.0000001; /* 0.01 is too big */

static int debug_flag = 0;

int main(int argc, char *argv[]) ;
static int get_option(int argc, char *argv[]) ;

char *Progname ;

static int normflag = 0; /* normalize input volume */

/* Values lower than threshold are considered as background, and not counted
 * in clustering
 */
static double noise_threshold = 1.0; /* threshold for background noise */

static double tolerance = 0.01; /* Convergence criterion */

static int num_classes = 3; /* default number of classes */

static int clear_dura = 0; 

static int max_iters = 100; /* defualt maximal iteration number */

static int whole_volume = 0; /* when LDA, project whole volume */
                             /* Is this necessary? it may affect the scaling
                              * and reduce dynamic range of useful regions
			      * But without it, how do I measure background 
			      * noise; but who cares about background noise!
			      */

static int hard_segmentation = 0;                            

static char *label_fname = NULL; /* filename for segmentation */

static int conform = 1 ;

static int mask_subcortical = 0;

static int SYNTH_ONLY = 0;

static int rescale = 0;

static int fuzzy_lda = 0; /* whether use Fuzzy cov and fuzzy centroid for LDA */

static double mybeta = 0.5; /*weight for MRF */

/* eps and lambda are used for covariance regularization */
static double eps = 1e-30;
static double lambda = 0.1;
static int regularize = 0;

/* Clustering is performed only within ROI */
static char *mask_fname = NULL; /* filename for ROI mask */

void indexx(unsigned long n, float arr[], unsigned long indx[]);

static void usage_exit(int code) ;

MRI *MRInormalizeXH(MRI *mri_src, MRI *mri_dst, MRI *mri_mask);

double NbhdLikelihood(MRI **mri_mem, MRI *mri_mask, int x, int y, int z, int label, int num_classes);

void fuzzyLDAweights(float *weights, MATRIX **F, float **centroids, int classID1, int classID2, float size1, float size2,  int nvolumes_total);

void update_centroids1D(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int num_classes);

void MRI_FCM(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int  num_classes);

void MRI_EM(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int  num_classes);

void update_centroids(MRI **mri_flash, MRI **mri_mem, MRI **mri_likelihood, MRI *mri_mask, float **centroids, int nvolumes_total, int num_classes);

void update_F(MATRIX **F, MRI **mri_flash, MRI **mri_mem, MRI **mri_lihood, MRI *mri_mask, float **centroids, int nvolumes_total, int num_classes);

void compute_detF(MATRIX **F, double *detF, int num_classes);

void compute_inverseF(MATRIX **F, int num_classes);

double distancems(MRI **mri_flash, float *centroids, MATRIX *F, double detF, int x, int y, int z);

void  update_LDAmeans(MRI **mri_flash, MRI **mri_mem, MRI *mri_mask, float *LDAmean1, float *LDAmean2, int nvolumes_total, int classID1, int classID2, float threshold);

void computeLDAweights(float *weights, MRI **mri_flash, MRI **mri_mem, MRI *mri_mask, float *LDAmean1, float *LDAmean2, int nvolumes_total, int classID1, int classID2, float threshold);


static int InterpMethod = SAMPLE_TRILINEAR;  /*E* prev default behavior */
static int sinchalfwindow = 3;
static int ldaflag = 0; /* Using LDA methods to synthesize volume */
static int class1 = 0; /* to be used for LDA */ 
static int class2 = 0; /* to be used for LDA */ 

#define MAX_IMAGES 200

int
main(int argc, char *argv[])
{
  char   **av, *in_fname, fname[100];
  int    ac, nargs, i, j,  x, y, z, width, height, depth, iter, c;
  MRI    *mri_flash[MAX_IMAGES], *mri_mem[MAX_CLASSES], *mri_mask, *mri_label;
  MRI *mri_mem_old[MAX_CLASSES];
  MRI *mri_tmp = NULL;
  char   *out_prefx ;
  int    msec, minutes, seconds, nvolumes, nvolumes_total, label ;
  struct timeb start ;
  MATRIX *F[MAX_CLASSES];
  float *centroids[MAX_CLASSES];
  float centroids1D[MAX_CLASSES];
  float max_val, min_val, value;
  float max_change = 1000.0;
  double detF[MAX_CLASSES]; /* determinant of the covariance matrix */
  double distance2, sum_of_distance; /* actually the inverse distance */
  double oldMems[MAX_CLASSES];
  float classSize[MAX_CLASSES];
  int brainsize;

  int nx, ny, nz, nc;

  double nbhdP;

  float *LDAmean1, *LDAmean2, *LDAweight;
  int class_dura;

  /* Used for sorting using Numerical recipe */
  float NRarray[MAX_CLASSES+1];
  unsigned long NRindex[MAX_CLASSES+1];
  int indexmap[MAX_CLASSES + 1];

  /* rkt: check for and handle version tag */
  nargs = handle_version_option (argc, argv, "$Id: mri_ms_EM.c,v 1.3 2005/02/14 20:21:32 xhan Exp $", "$Name:  $");
  if (nargs && argc - nargs == 1)
    exit (0);
  argc -= nargs;
  
  Progname = argv[0] ;
  ErrorInit(NULL, NULL, NULL) ;
  DiagInit(NULL, NULL, NULL) ;

  if(ldaflag){
    if(class1 == class2 || class1 < 1 || class1 > num_classes
	 || class2 < 1 || class2 > num_classes){
      ErrorExit(ERROR_NOFILE, "%s: The two class IDs must be within [1, %d]",
                Progname, num_classes) ;
    }
    
  }

  TimerStart(&start) ;
  
  ac = argc ;
  av = argv ;
  for ( ; argc > 1 && ISOPTION(*argv[1]) ; argc--, argv++)
  {
    nargs = get_option(argc, argv) ;
    argc -= nargs ;
    argv += nargs ;
  }
  
  if (argc < 2)
    usage_exit(1) ;

  out_prefx = argv[argc-1] ;

  printf("command line parsing finished\n");

  //////////////////////////////////////////////////////////////////////////////////
  /*** Read in the input multi-echo volumes ***/
  nvolumes = 0 ;
  for (i = 1 ; i < argc-1 ; i++){
    in_fname = argv[i] ;
    printf("reading %s...\n", in_fname) ;
    
    mri_flash[nvolumes] = MRIread(in_fname) ;
    if (mri_flash[nvolumes] == NULL)
      ErrorExit(ERROR_NOFILE, "%s: could not read volume %s",
                Progname, in_fname) ;
    /* conform will convert all data to UCHAR, which will reduce data resolution*/
    printf("%s read in. \n", in_fname) ;
    if (conform){
      printf("embedding and interpolating volume\n") ;
      mri_tmp = MRIconform(mri_flash[nvolumes]) ;
      MRIfree(&mri_flash[nvolumes]);
      mri_flash[nvolumes] = mri_tmp ; mri_tmp = 0;
    }

    /* Change all volumes to float type for convenience */
    if(mri_flash[nvolumes]->type != MRI_FLOAT){
      printf("Volume %d type is %d\n", nvolumes+1, mri_flash[nvolumes]->type);
      printf("Change data to float type \n");
      mri_tmp = MRIchangeType(mri_flash[nvolumes], MRI_FLOAT, 0, 1.0, 1);
      MRIfree(&mri_flash[nvolumes]);
      mri_flash[nvolumes] = mri_tmp; mri_tmp = 0; //swap
    }

    nvolumes++ ;
  }

  printf("All data read in\n");

  ///////////////////////////////////////////////////////////////////////////
  nvolumes_total = nvolumes ;   /* all volumes read in */

  for (i = 0 ; i < nvolumes ; i++){
    for (j = i+1 ; j < nvolumes ; j++){
      if ((mri_flash[i]->width != mri_flash[j]->width) ||
	  (mri_flash[i]->height != mri_flash[j]->height) ||
	  (mri_flash[i]->depth != mri_flash[j]->depth))
	ErrorExit(ERROR_BADPARM, "%s:\nvolumes %d (type %d) and %d (type %d) don't match (%d x %d x %d) vs (%d x %d x %d)\n",
		  Progname, i, mri_flash[i]->type, j, mri_flash[j]->type, mri_flash[i]->width, 
		  mri_flash[i]->height, mri_flash[i]->depth, 
		  mri_flash[j]->width, mri_flash[j]->height, mri_flash[j]->depth) ;
    }
  }

  width = mri_flash[0]->width;
  height = mri_flash[0]->height;
  depth = mri_flash[0]->depth;
  
  if(label_fname != NULL){
    mri_label = MRIread(label_fname);
    if(!mri_label)
      ErrorExit(ERROR_NOFILE, "%s: could not read input volume %s\n",
		Progname, label_fname);
    
    if((mri_label->width != mri_flash[0]->width) ||
       (mri_label->height != mri_flash[0]->height) ||
       (mri_label->depth != mri_flash[0]->depth))
      ErrorExit(ERROR_BADPARM, "%s: label volume size doesn't match data volumes\n", Progname);
    
    /* if(mri_label->type != MRI_UCHAR)
       ErrorExit(ERROR_BADPARM, "%s: label volume is not UCHAR type \n", Progname); */
  }
  
  if(mask_fname != NULL){
    mri_mask = MRIread(mask_fname);
    if(!mri_mask)
      ErrorExit(ERROR_NOFILE, "%s: could not read input volume %s\n",
		Progname, mask_fname);

    if((mri_mask->width != mri_flash[0]->width) ||
       (mri_mask->height != mri_flash[0]->height) ||
       (mri_mask->depth != mri_flash[0]->depth))
      ErrorExit(ERROR_BADPARM, "%s: mask volume size doesn't macth data volumes\n", Progname);

    if(mri_mask->type != MRI_UCHAR)
      ErrorExit(ERROR_BADPARM, "%s: mask volume is not UCHAR type \n", Progname);
  }else{
    mri_mask = MRIalloc(mri_flash[0]->width, mri_flash[0]->height, mri_flash[0]->depth, MRI_UCHAR);
    MRIcopyHeader(mri_flash[0], mri_mask);

    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    MRIvox(mri_mask, x, y,z) = 1;
	  }

    if(label_fname != NULL){
      mri_tmp = MRIclone(mri_mask, NULL);
      printf("Use label volume to define regions to be segmented. \n");
      for(z=0; z < depth; z++)
	for(y=0; y< height; y++)
	  for(x=0; x < width; x++)
	    {
	      label = (int)MRIgetVoxVal(mri_label, x, y,z,0);

	      if(label <= 0)
		MRIvox(mri_tmp, x, y,z) = 0;
	      else
		MRIvox(mri_tmp, x, y,z) = 1;
	    }
      /* dilate the region with label by 2 */
      mri_mask = MRIdilateLabelUchar(mri_tmp, mri_mask, 1, 2);
      MRIfree(&mri_tmp);
    }
    
    /* Compute the mask from the first input volume */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(MRIFvox(mri_flash[0], x, y, z) < (float) noise_threshold)
	      MRIvox(mri_mask, x, y,z) = 0;

	    if(mask_subcortical == 1 && label_fname != NULL){
	      label = (int)MRIgetVoxVal(mri_label, x, y,z,0);
	      /* if((label >= 9 && label <= 12) || (label >= 48 && label <= 51) || label ==4 || label == 5 || label == 43 || label == 44 ) */
	      if((label >= 9 && label <= 12) || (label >= 48 && label <= 51) )
		MRIvox(mri_mask, x, y,z) = 0; 
	    }
	  }
    
  }

  /* Allocate memory for likelihood volumes */
  for(i=0; i < num_classes; i++){
    mri_mem_old[i] = MRIclone(mri_flash[0], NULL);
  }

  brainsize = 0;
  for(z=0; z < depth; z++)
    for(y=0; y< height; y++)
      for(x=0; x < width; x++)
	{
	  if(MRIvox(mri_mask, x, y, z) <= 0) continue;

	  /* since mri_mem_old is now likelihood, it need be initialized */
	  for(i=0; i < num_classes; i++){
	    MRIFvox(mri_mem_old[i],x,y,z) = 1.0;
	  }
	  
	  brainsize++;
	}
  
  printf("Brain size = %d\n", brainsize);
  if(brainsize < 1){
    printf("region to be segment has zero size. exit.\n");
    exit(1);
  }

  /* Normalize input volumes */
  if(normflag){
    printf("Normalize input volumes to zero mean, variance 1\n");
    for(i=0; i <nvolumes_total; i++){
      mri_flash[i] = MRInormalizeXH(mri_flash[i], mri_flash[i], mri_mask);
    }
    printf("Normalization done.\n");
  }

  /* Allocate memory for membership volumes */
  for(i=0; i < num_classes; i++){
    mri_mem[i] = MRIclone(mri_flash[0], NULL);
    //    mri_mem_old[i] = MRIclone(mri_flash[0], NULL);
    centroids[i] = (float *)malloc(nvolumes_total*sizeof(float));
    F[i] = (MATRIX *)MatrixAlloc(nvolumes_total, nvolumes_total, MATRIX_REAL);
    classSize[i] = 1.0/(float)num_classes;
  }

  /* initialize F's to identity matrices */
  for(c=0; c < num_classes; c++){
    for(i=1; i <= nvolumes_total; i++){
      for(j=i+1; j <= nvolumes_total; j++){
	F[c]->rptr[i][j] = 0.0;
	F[c]->rptr[j][i] = 0.0;
      }
      F[c]->rptr[i][i] = 1.0;
    }
    detF[c] = 1.0;
  }    

  /* Initialize by performing EM on first volume */
  MRIvalRange(mri_flash[0], &min_val, &max_val);
  for(i=0; i < num_classes; i++){
    centroids1D[i] = min_val + (i+1.0)*(max_val - min_val)/(1.0 + num_classes);
  }

  printf("Perform 1D EM on the first volume\n");
  MRI_FCM(mri_flash[0], mri_mem, mri_mask, centroids1D, num_classes);
  //  MRI_EM(mri_flash[0], mri_mem, mri_mask, centroids1D, num_classes);
  printf("1D EM finished.\n");

  /* Use the membership function obtained above to get initial centroids
   * for the multi-dimensional clustering 
   */

  /* Perform iterative fuzzy-clustering until convergence */
  iter = 0; max_change = 10000.0;
  while(max_change > tolerance && iter < max_iters){
    iter++; max_change = 0.0;

    printf("iteration # %d:\n", iter);
    /* Update centroids */
    update_centroids(mri_flash, mri_mem, mri_mem_old, mri_mask, centroids, nvolumes_total, num_classes);

    for(c=0; c < num_classes; c++){
      printf("Centroids for class %d:", c+1);
      for(i=0; i < nvolumes_total; i++)
	printf(" %g ", centroids[c][i]);
      printf("\n");
    }

    /* Update covariance matrix and prior class-probability */
    /* Compute fuzzy covariance matrix F, one for each class */
    update_F(F, mri_flash, mri_mem, mri_mem_old, mri_mask, centroids, nvolumes_total, num_classes);
    
    if(fix_class_size ==1){
      for(c=0; c < num_classes; c++)
	classSize[c] = 1;
      //    classSize[0] = 0.09;
      //classSize[1] = 0.18;
      //classSize[2] = 0.33;
      //classSize[3] = 0.33;
    } else{
      /* update class size */
      for(c=0; c < num_classes; c++)
	classSize[c] = 0;
      for(z=0; z < depth; z++)
	for(y=0; y< height; y++)
	  for(x=0; x < width; x++){
	    if(MRIvox(mri_mask, x, y, z) == 0) continue;
	    for(c=0; c < num_classes; c++){
	      classSize[c] += MRIFvox(mri_mem[c], x, y, z);
	    }
	  }
      
      for(c=0; c < num_classes; c++)
	classSize[c] /= (float)brainsize;
    }
    
    if(nvolumes_total == 1){
      for(c=0; c < num_classes; c++){
	
	F[c]->rptr[1][1] = 1.0/F[c]->rptr[1][1];
	detF[c] = F[c]->rptr[1][1]; /* detF is actually the det of inverseF */
      }
    }
    else{
      /* Compute the inverse, and store in the original place */
      compute_inverseF(F, num_classes);

      /* compute determinant of inversed F */
      compute_detF(F, detF, num_classes);

      /* Use identity */
      /* This will lead to every voxel belongs equally to all three classes */
      /* for(c=0; c < num_classes; c++){
	 for(i=1; i <= nvolumes_total; i++){
	 for(j=i+1; j <= nvolumes_total; j++){
	 F[c]->rptr[i][j] = 0.0;
	 F[c]->rptr[j][i] = 0.0;
	 }
	 F[c]->rptr[i][i] = 1.0;
	 }
	 detF[c] = 1.0;
	 }    
      */
    }

    for(c=0; c < num_classes; c++)
      printf("classSize[%d] = %g, DetF[%d] = %g\n", c, classSize[c], c, detF[c]);
  

    /* Update membership function */
    /* first copy old_ones */
    /* I will use mri_mem_old to store likelihood */
    /*
      for(c=0; c < num_classes; c++){
      mri_mem_old[c] = MRIcopy(mri_mem[c], mri_mem_old[c]);
      }
    */
    
    /* Need to change this to Gauss-Seidel, otherwise it's too slow */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++) {
	  if(MRIvox(mri_mask, x, y, z) == 0) continue;	  
	  
	  sum_of_distance = 0;
	  /* Compute distance */
	  for(c=0; c < num_classes; c++){
	    /* record old membership values */
	    oldMems[c] = MRIFvox(mri_mem[c], x, y, z);
	    distance2 = distancems(mri_flash, centroids[c], F[c], detF[c], x, y, z);
	    distance2 = exp(-0.5*distance2);
	    /* Distance2 now is the likelihood, store it */
	    MRIFvox(mri_mem_old[c], x, y, z) = distance2;

	    distance2 *= (classSize[c]*sqrt(detF[c])); 
	    
	    /* Compute neighborhood PDF */
	    /* Need to find the ML estimate for each neighbor and then sum up
	       the prior. This will be very time-consuming; easier if use atlas
	       To make it faster, have to store previous membership functions
	    */

	    // nbhdP = 100* NbhdLikelihood(mri_mem_old, mri_mask, x, y, z, c, num_classes);
	    /* Use current membership */
	    /* Gives better convergence rate */
	    /* Maybe a Gauss-seidel iteration is even better */
	    nbhdP = 100* NbhdLikelihood(mri_mem, mri_mask, x, y, z, c, num_classes);

	    if(debug_flag && x == Gx && y == Gy && z == Gz){
	      printf("c= %d, nbhdP =%g \n", c, nbhdP);
	    }

	    distance2 *= nbhdP;

	    /* printf("distance2 = %g\n", distance2); */
	    MRIFvox(mri_mem[c], x, y, z) = distance2;
	    sum_of_distance += distance2;
	  }

	  if(sum_of_distance <= 1e10){ /* Outlier */
	    sum_of_distance = 1.0;
	    //	    printf("(x,y,z) = (%d,%d,%d), sum_of_distance = %g\n",x,y,z, sum_of_distance);
	    // ErrorExit(ERROR_BADPARM, "%s: overflow in computing membership function.\n", Progname);		    
	  }

	  for(c=0; c < num_classes; c++){
	    /* borrow distance2 here */
	    distance2 = MRIFvox(mri_mem[c], x, y, z)/sum_of_distance;
	    MRIFvox(mri_mem[c], x, y, z) = distance2;

	    if(debug_flag && x == Gx && y == Gy && z == Gz){
	      printf("c= %d, memship =%g \n", c, distance2);
	    }

	    distance2 -= oldMems[c];
	    if(distance2 < 0) distance2 = -distance2;


	    //	    if(distance2 > 0.99 && iter > 1){
	    // printf("oldmem = %g, newmem =%g, sum_of_distance = %g\n", oldMems[c], MRIFvox(mri_mem[c], x, y, z), sum_of_distance);
	    //}

	    if(max_change < distance2){
	      max_change = distance2;
	      nx = x, ny = y, nz = z;
	      nc = c;
	    }
	  }
	  
	  
	} /* end of all data points */
    
    printf("maxchange = %g (at (%d,%d,%d))\n", max_change, nx, ny, nz);
    /* printf("oldmem(%d) = %g, new = %g\n", nc, MRIFvox(mri_mem_old[nc], nx, ny, nz),MRIFvox(mri_mem[nc], nx, ny, nz)); */ // no longer valid
    
  } /* end of while-loop */
  
  

  /* How to map the multi-dimensional data to 1D using the class centers and
   * distance may worth some further investigation.
   * May need to optimize the location of the 1D centroids so as
   * to minimize the distance distortion. Or can I set the 1D centroid,
   * and only concerns about the mapping?
   */
  /* Will the problem becomes easier if my aim is to generate two volumes
   * one for depicting GM/WM contrast, one for GM/CSF contrast? should be!
   * Note that the centroid has no ordering in the nD space, but in 1D, they
   * clearly have an ordering!
   */
  /* Maybe the mapping is not important. Important is that the clustering is 
   * good. If it is, then most voxels will be close to one centroid! So the
   * mapping seems indeed not important! If the clustering is bad, most
   * pixel will be fuzzy (membership close to 0.5), then no matter how to 
   * choose the map. The synthesized image will have poor contrast!
   */


  /* Update centroids using first image and use it in corting */
  update_centroids1D(mri_flash[0], mri_mem, mri_mask, centroids1D, num_classes);
  
  for(i=0; i < num_classes; i++){
    NRarray[i+1] = centroids1D[i]; 
    printf("centroids1D[%d] = %g\n",i, centroids1D[i]);
  }
  
  /* NR sort in ascending order */
  indexx(num_classes, NRarray, NRindex);

  printf("Sorted centroids\n");
  for(i=1; i <= num_classes; i++){
    printf("NRindex[%d] = %ld\n", i, NRindex[i]);
    indexmap[NRindex[i]-1] = i;
  }
  for(i=0; i < num_classes; i++){
    printf("indexmap[%d] = %d\n", i, indexmap[i]);
  }

  /* Compute class size */
  for(c=0; c < num_classes; c++)
    classSize[c] = 0;
  for(z=0; z < depth; z++)
    for(y=0; y< height; y++)
      for(x=0; x < width; x++){
	if(MRIvox(mri_mask, x, y, z) == 0) continue;
	for(c=0; c < num_classes; c++){
	  classSize[c] += MRIFvox(mri_mem[c], x, y, z);
	}
      }
  
  if(classSize[NRindex[1]] > classSize[NRindex[2]]){
    printf("Switch to make sure least size is dura, and as final label 1\n");
    i = NRindex[1] -1; j = NRindex[2] - 1;
    c = NRindex[1]; NRindex[1] = NRindex[2]; NRindex[2] = c;
    indexmap[i] = 2; indexmap[j] = 1;
  } 

  if(num_classes == 4){
    /* compute the size of class 1 and class 2, and assign the one with smaller size to class 1 (dura) */
    printf("Is this meaningful?\n");
    
  }

  if(hard_segmentation){
    MRI *mri_seg = NULL;
        
    mri_seg = MRIcopy(mri_mask, mri_seg);
    
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(MRIvox(mri_mask, x, y, z) == 0) continue;
	    i = 0;
	    value = MRIFvox(mri_mem[0], x, y, z);   
	    for(j=1; j < num_classes; j++){
	      if(value < MRIFvox(mri_mem[j], x, y, z)){
		i = j;
		value = MRIFvox(mri_mem[j], x, y, z);
	      }		
	    }
	    
	    MRIvox(mri_seg, x, y, z) = indexmap[i];
	    
	  }
    
    sprintf(fname,"%sEM_hseg.mgz", out_prefx);
    MRIwrite(mri_seg, fname);    
    MRIfree(&mri_seg);
  }

  if(!ldaflag){    
    /* synthesize the 1D image */
    for(i=0; i < num_classes; i++){
      j = NRindex[i+1] - 1;
      centroids1D[j] = 15.0 + i*(225.0 - 15.0)/(num_classes-1.0);
    }
    
    if(rescale){
      /* Nonlinear scale of the membership functions */
      for(z=0; z < depth; z++)
	for(y=0; y< height; y++)
	  for(x=0; x < width; x++)
	    {
	      if(MRIvox(mri_mask, x, y, z) == 0) continue;
	      
	      /* Scale by a sin-function */
	      for(i=0; i < num_classes; i++){
		value = MRIFvox(mri_mem[i], x, y, z);
		MRIFvox(mri_mem[i], x, y, z) = 0.5*(sin((value-0.5)*3.14159265) + 1.0);
	      }
	      
	      /* Renormalize to sum up to 1 */
	      value = 0.0;
	      for(i=0; i < num_classes; i++){
		value += MRIFvox(mri_mem[i], x, y, z);
	      }
	      
	      for(i=0; i < num_classes; i++){
		MRIFvox(mri_mem[i], x, y, z) /= (value + 0.0000000001);
	      }
	    }
    }
    
    /* borrow mri_mask for the output image */
    /* Linear interpolation will blur things, better performed in 
     * float image. So the conformal switch should be one! 
     */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(MRIvox(mri_mask, x, y, z) == 0) continue;
	    
	    value = 0.0;
	    for(i=0; i < num_classes; i++){
	      value += MRIFvox(mri_mem[i], x, y, z)*centroids1D[i];
	    }
	    
	    MRIvox(mri_mask, x, y, z) = (BUFTYPE) value;
	  }
    
    /* Output synthesized volume */
    sprintf(fname,"%sEM_combined.mgz", out_prefx);
    MRIwrite(mri_mask, fname);
  }else{
    /* Using LDA method for synthesize image */
    printf("Start LDA processing ...\n");
    /* map the output class ID to the true class ID */
    class1 = NRindex[class1] - 1;
    class2 = NRindex[class2] - 1;
    if(clear_dura)
      class_dura = NRindex[1] - 1;
    else
      class_dura = -1;

    printf("class1 = %d, class2 = %d, class_dura = %d\n", class1, class2, class_dura);

    LDAmean1 = (float *)malloc(nvolumes_total*sizeof(float));
    LDAmean2 = (float *)malloc(nvolumes_total*sizeof(float));
    LDAweight = (float *)malloc(nvolumes_total*sizeof(float));

    if(fuzzy_lda == 0){
      /* Compute class means */
      update_LDAmeans(mri_flash, mri_mem, mri_mask, LDAmean1, LDAmean2, nvolumes_total, class1, class2, MEMTHRESHOLD); /* 0.7 is a threshold, as to what voxels will be counted as in class1 */
      printf("class means computed \n");
      
      /* Compute Fisher's LDA weights */
      computeLDAweights(LDAweight, mri_flash, mri_mem, mri_mask, LDAmean1, LDAmean2, nvolumes_total, class1, class2, MEMTHRESHOLD);
    }else{
      /* Use fuzzy covariance matrix and centroids to compute LDA weights */
      update_F(F, mri_flash, mri_mem, mri_mem_old, mri_mask, centroids, nvolumes_total, num_classes);

      fuzzyLDAweights(LDAweight, F, centroids, class1, class2, 1, 1, nvolumes_total);

    }

    printf("LDA weights are: \n");
    for(i=0; i < nvolumes_total; i++){
      printf("%g ", LDAweight[i]);
    }
    printf("\n");
    /* linear projection of input volumes to a 1D volume */
    min_val = 10000.0;
    max_val = -10000.0;
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(whole_volume == 0 && MRIvox(mri_mask, x, y, z) == 0) continue;
	    
	    value = 0.0;
	    
	    if(clear_dura == 1 &&  MRIFvox(mri_mem[class_dura], x, y, z) > 0.25){
	      /* value = 0.0; */ /* Note that 0 is not minimum value! 
			      So this won't work */
	      MRIvox(mri_mask, x , y, z) = 0;
	      continue; /* This agrees with later processing */
	    }
	    else{
	      for(i=0; i < nvolumes_total; i++){
		value += MRIFvox(mri_flash[i], x, y, z)*LDAweight[i];
	      }
	      
	    }

	    if(max_val < value) max_val = value;
	    if(min_val > value) min_val = value;

	    /* Borrow mri_flash[0] to store the float values first */
	    MRIFvox(mri_flash[0], x, y, z) = value;
	  }
    
    printf("max_val = %g, min_val = %g \n", max_val, min_val);

    /* Scale output to [0, 255] */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(whole_volume == 0 && MRIvox(mri_mask, x, y, z) == 0) continue;
	    
	    value = (MRIFvox(mri_flash[0], x, y, z) - min_val)*255.0/(max_val - min_val) + 0.5; /* +0.5 for round-off */
	    
	    if(value > 255.0) value = 255.0;
	    if(value < 0) value = 0;

	    /* Borrow mri_flash[0] to store the float values first */
	    MRIvox(mri_mask, x, y, z) = (BUFTYPE) value;
	  }

    /* Output synthesized volume */
    sprintf(fname,"%sLDA_combined.mgz", out_prefx);
    MRIwrite(mri_mask, fname);

    free(LDAmean1);
    free(LDAmean2);
    free(LDAweight);
  }
  
  if(SYNTH_ONLY == 0){

    /* output membership functions */
    printf("Convert membership function to BYTE and write out\n");
    mri_tmp = MRIclone(mri_mask, NULL);

    for(c=0; c < num_classes; c++){
            
      /*
      printf("Output membership volume for class %d\n",c) ;
      mri_tmp = MRIconform(mri_mem[c]) ;
      */
      for(z=0; z < depth; z++)
	for(y=0; y< height; y++)
	  for(x=0; x < width; x++){
	    if(MRIvox(mri_mask, x, y, z) == 0){
	      MRIvox(mri_tmp,x,y,z) = 0;
	      continue;
	    }
	    value =  255.0*MRIFvox(mri_mem[c], x, y, z) + 0.5;
	    if(value > 255.0) value = 255;
	    if(value < 0.0) value = 0;
	    
	    MRIvox(mri_tmp,x,y,z) = (unsigned char)value;
	  }

      sprintf(fname,"%sEM_mem%d.mgz", out_prefx, indexmap[c]);
      MRIwrite(mri_tmp, fname);

    }

    MRIfree(&mri_tmp);
  }
  
  msec = TimerStop(&start) ;
  seconds = nint((float)msec/1000.0f) ;
  minutes = seconds / 60 ;
  seconds = seconds % 60 ;
  printf("parameter estimation took %d minutes and %d seconds.\n", minutes, seconds) ;

  MRIfree(&mri_mask);

  if(label_fname != NULL){
    MRIfree(&mri_label);
  }      

  for(i=0; i < num_classes; i++){
    MRIfree(&mri_mem[i]);
    MRIfree(&mri_mem_old[i]);
    free(centroids[i]);
    MatrixFree(&F[i]);
  }

  for(i=0; i < nvolumes_total; i++){
    MRIfree(&mri_flash[i]);
  }
  exit(0);
}

/*----------------------------------------------------------------------
            Parameters:

           Description:
----------------------------------------------------------------------*/
static int
get_option(int argc, char *argv[])
{
  int  nargs = 0 ;
  char *option ;
  
  option = argv[1] + 1 ;            /* past '-' */
  if (!stricmp(option, "debug_voxel"))
    {
      Gx = atoi(argv[2]) ;
      Gy = atoi(argv[3]) ;
      Gz = atoi(argv[4]) ;
      debug_flag = 1;
      nargs = 3 ;
      printf("debugging voxel (%d, %d, %d)...\n", Gx, Gy, Gz) ;
    }
  else if (!stricmp(option, "conform"))
    {
      conform = 1 ;
      printf("interpolating volume to be isotropic 1mm^3\n") ;
    }
  else if (!stricmp(option, "fuzzy_lda"))
    {
      fuzzy_lda = 1 ;
      printf("Using fuzzy LDA weighting scheme\n") ;
    }
  else if (!stricmp(option, "clear_dura"))
    {
      clear_dura = 1 ;
      printf("Remove voxels belongs to second class\n") ;
    }
  else if (!stricmp(option, "whole_volume"))
    {
      whole_volume = 1 ;
      printf("Synthesize background region too (if LDA)\n") ;
    }
  else if (!stricmp(option, "lda"))
    {
      ldaflag = 1;
      class1 = atoi(argv[2]) ;
      class2 = atoi(argv[3]) ;
      nargs = 2;
      printf("Using LDA method to generate synthesized volume (%d, %d) \n", class1, class2);
    }
  else if (!stricmp(option, "synthonly"))
    {
      SYNTH_ONLY = 1;
      printf("Do not output membership functions\n") ;
    }
  else if (!stricmp(option, "norm"))
    {
      normflag = 1;
      printf("Normalize input volumes to N(0,1)\n");
    }
  else if (!stricmp(option, "mask"))
    {
      mask_fname = argv[2];
      printf("using %s as mask for regions of interest \n", mask_fname);
      nargs = 1;
    }
  else if (!stricmp(option, "rescale"))
    {
      rescale = 1;
      printf("Rescale the membership function to improve contrast.\n") ;
    }
  else if (!stricmp(option, "hard_seg"))
    {
      hard_segmentation = 1;
      printf("Output a hard segmentation to out_pre.hseg \n") ;
    }
  else if (!stricmp(option, "window"))
    {
      printf("window option not implemented\n");
      /*E* window_flag = 1 ; */
    }
	   
  /*E* Interpolation method.  Default is trilinear, other options are
    nearest, cubic, sinc.  You can say -foo or -interp foo.  For sinc,
    you can say -interp sinc 3 or -interp sinc -hw 3 or -sinc 3 or
    -sinc -hw 3.  Maybe -hw 3 should imply sinc, but right now it
    doesn't.  */

  else if (!stricmp(option, "st") ||
	   !stricmp(option, "sample") ||
	   !stricmp(option, "sample_type") ||
	   !stricmp(option, "interp"))
    {
      InterpMethod = MRIinterpCode(argv[2]) ;
      nargs = 1;
      if (InterpMethod==SAMPLE_SINC)
	{
	  if ((argc<4) || !strncmp(argv[3],"-",1)) /*E* i.e. no sinchalfwindow value supplied */
	    {
	      printf("using sinc interpolation (default windowwidth is 6)\n");
	    }
	  else
	    {
	      sinchalfwindow = atoi(argv[3]);
	      nargs = 2;
	      printf("using sinc interpolation with windowwidth of %d\n", 2*sinchalfwindow);
	    }
	}
    }
  else if (!stricmp(option, "sinc"))
    {
      InterpMethod = SAMPLE_SINC;
      if ((argc<3) || !strncmp(argv[2],"-",1)) /*E* i.e. no sinchalfwindow value supplied */
	{
	  printf("using sinc interpolation (default windowwidth is 6)\n");
	}
      else
	{
	  sinchalfwindow = atoi(argv[2]);
	  nargs = 1;
	  printf("using sinc interpolation with windowwidth of %d\n", 2*sinchalfwindow);
	}
    }
  else if (!stricmp(option, "sinchalfwindow") ||
	   !stricmp(option, "hw"))
    {
      /*E* InterpMethod = SAMPLE_SINC; //? */
      sinchalfwindow = atoi(argv[2]);
      nargs = 1;
      printf("using sinc interpolation with windowwidth of %d\n", 2*sinchalfwindow);
    }
  else if(!stricmp(option, "beta"))
    {
      mybeta = atof(argv[2]);
      printf("weight for MRF = %g\n", mybeta);
      nargs = 1;
    }
  else if(!stricmp(option, "regularize"))
    {
      regularize = 1;
      printf("Regularize the covariance matrix\n");
    }
  else if (!stricmp(option, "label"))
    {
      label_fname = argv[2];
      printf("using %s as segmentation volume \n", label_fname);
      nargs = 1;
    }
  else if (!stricmp(option, "mask_subcortical"))
    {
      mask_subcortical = 1;
      printf("mask subcortical GM region too. \n");
    }
  else if (!stricmp(option, "trilinear"))
    {
      InterpMethod = SAMPLE_TRILINEAR;
      printf("using trilinear interpolation\n");
    }
  else if (!stricmp(option, "cubic"))
    {
      InterpMethod = SAMPLE_CUBIC;
      printf("using cubic interpolation\n");
    }
  else if (!stricmp(option, "nearest"))
  {
      InterpMethod = SAMPLE_NEAREST;
      printf("using nearest-neighbor interpolation\n");
    }
  else if (!stricmp(option, "noconform"))
    {
      conform = 0 ;
      printf("inhibiting isotropic volume interpolation\n") ;
    }
  else switch (toupper(*option))
    {
    case 'M':
      num_classes = atoi(argv[2]) ;
      nargs = 1 ;
      printf("Number of classes=%d\n", num_classes) ;
      if(num_classes > MAX_CLASSES)
	ErrorExit(ERROR_BADPARM, "%s: too many desired classes.\n", Progname);		
      break ;
    case 'T':
      noise_threshold = atof(argv[2]) ;
      printf("Threshold for background noise = %g\n", noise_threshold) ;
      printf("The threshold is applied on first input volume.\n") ;
      nargs = 1 ;
      break ;
    case 'E':
      tolerance = atof(argv[2]) ;
      printf("End tolerance = %g\n", tolerance) ;
      nargs = 1 ;
      break ;
    case 'R':
      max_iters = atoi(argv[2]) ;
      printf("Maximum number of iterations = %d\n", max_iters) ;
      nargs = 1 ;
      break ;
    case '?':
    case 'U':
      usage_exit(0) ;
      break ;
    default:
      printf("unknown option %s\n", argv[1]) ;
      exit(1) ;
      break ;
    }
  
  return(nargs) ;
}
/*----------------------------------------------------------------------
  Parameters:
  
  Description:
  ----------------------------------------------------------------------*/
static void
usage_exit(int code)
{
  printf("usage: %s [options] <volume> ... <output prefix>\n", Progname) ;
  printf("This program takes an arbitrary # of FLASH images as input,\n"
	 "and performs EM-segmentation on the multidimensional\n"
	 "intensity space.\n");
  printf("Options includes:\n");
  printf("\t -mask fname to set the brain mask volume\n");
  printf("\t -M # to set the number of classes \n");
  printf("\t -E # to set the convergence tolerance \n");
  printf("\t -R # to set the max number of iterations \n");
  printf("\t -T # to set the threshold for background \n");
  printf("\t -norm to normalize input volumes before clustering \n");
  printf("\t -conform to conform input volumes (brain mask typically already conformed) \n");
  printf("\t -noconform to prevent conforming to COR \n");
  printf("\t -synthonly to prevent output membership functions \n");
  exit(code) ;
  
}

double NbhdLikelihood(MRI **mri_mem, MRI *mri_mask, int x, int y, int z, int label, int num_classes){
  int c, index, nx, ny, nz, nlabel;
  int width, height, depth;

  double p, maxp, tmpp, currentp;
  int total, diff;

  width = mri_mask->width;
  height = mri_mask->height;
  depth = mri_mask->depth;

  currentp = MRIFvox(mri_mem[label], x, y, z);

  total = 0; diff = 0; p = 0;
  for(index = 0; index < 6; index++){
    nx = x + xoff[index];
    ny = y + yoff[index];
    nz = z + zoff[index];

    if(nx < 0 || nx >= width || ny < 0 || ny >= height || nz < 0 || nz >=depth)
      continue;
    if(MRIvox(mri_mask, nx, ny, nz) == 0) continue;
  
    total++;

    if(1){
      /* find the hard-label for this neighbor */
      nlabel = 0; maxp = MRIFvox(mri_mem[0], nx, ny, nz);
      for(c = 1; c < num_classes; c++){
	tmpp = MRIFvox(mri_mem[c], nx, ny, nz); 
	if(tmpp > maxp){
	  maxp = tmpp;
	  nlabel = c;
	}
      }
      
      if(nlabel != label) diff++;
    }
    else{ /* soft penalty, will tie more closely to a smoothing of probability itself*/ 
      /* This will make all p equal, and the hardsegmentation is more like noise */
      tmpp = currentp -  MRIFvox(mri_mem[label], nx, ny, nz);
      if(tmpp < 0) p -= tmpp;
      else p += tmpp;
    }
  }

  p = exp(-mybeta*diff/(total + 1e-10));

  return p;
}


void  update_LDAmeans(MRI **mri_flash, MRI **mri_mem, MRI *mri_mask, float *LDAmean1, float *LDAmean2, int nvolumes_total, int classID1, int classID2, float threshold){
  /* maybe I should design a Fuzzy LDA !! */

  int m, x, y, z, depth, height, width;
  double numer1, numer2, denom1, denom2;
  double mem;
  
  depth = mri_flash[0]->depth;
  width = mri_flash[0]->width;
  height = mri_flash[0]->height;
  
  for(m=0; m < nvolumes_total; m++){
    numer1 = 0; denom1 = 0;
    numer2 = 0; denom2 = 0;
    
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(MRIvox(mri_mask, x, y, z) > 0){
	      mem = MRIFvox(mri_mem[classID1], x, y, z);
	      if(mem >= threshold){
		numer1 += MRIFvox(mri_flash[m], x, y, z);
		denom1 += 1.0;
	      }
	      
	      mem = MRIFvox(mri_mem[classID2], x, y, z);
	      if(mem >= threshold){
		numer2 += MRIFvox(mri_flash[m], x, y, z);
		denom2 += 1.0;
	      }
	      
	    }
	    
	  }

    LDAmean1[m] = numer1/(denom1 + 0.0001);
    LDAmean2[m] = numer2/(denom2 + 0.0001);
  
  }
  
  return;
  
}

void fuzzyLDAweights(float *weights, MATRIX **F, float **centroids, int classID1, int classID2, float size1, float size2,  int nvolumes_total){


  int m1, m2;
  double denom, sumw;

  MATRIX *InvSW, *SW;

  SW = (MATRIX *)MatrixAlloc(nvolumes_total, nvolumes_total, MATRIX_REAL);    

  for(m1=1; m1 <= nvolumes_total; m1++){
    for(m2=m1; m2 <= nvolumes_total; m2++){
      SW->rptr[m1][m2] = F[classID1]->rptr[m1][m2] *size1 + F[classID2]->rptr[m1][m2] * size2;    /* index starts from 1 for matrix */
    }

    /* SW->rptr[m1][m1] += 0.00001; */ /* prevent SW to be singular */
  }

  /* Compute inverse of SW */
  InvSW = MatrixInverse(SW, NULL);
  
  if(InvSW == NULL){ /* inverse doesn't exist */
    ErrorExit(ERROR_BADPARM, "%s: singular fuzzy covariance matrix.\n", Progname);	          
  }
  
  /* Compute weights */
  denom = 0.0; sumw = 0.0;
  for(m1=1; m1 <= nvolumes_total; m1++){
    weights[m1-1]= 0.0;
    for(m2=1; m2 <= nvolumes_total; m2++){
      weights[m1-1] += InvSW->rptr[m1][m2] *(centroids[classID1][m2-1] - centroids[classID2][m2-1]);
    }
    sumw += weights[m1-1];
    denom += weights[m1-1]*weights[m1-1];
  }

  denom = sqrt(denom + 0.0000001);
  /* Normalized weights to have norm 1 */
  for(m1=1; m1 <= nvolumes_total; m1++){
    if(sumw > 0)
      weights[m1-1] /= denom;
    else
      weights[m1-1] /= -denom;
  }
  
  MatrixFree(&InvSW);
  
  MatrixFree(&SW);

  
  return;

}


void computeLDAweights(float *weights, MRI **mri_flash, MRI **mri_mem, MRI *mri_mask, float *LDAmean1, float *LDAmean2, int nvolumes_total, int classID1, int classID2, float threshold){

  int m1, m2, x, y, z, depth, height, width;
  double denom, sumw;
  double data1, data2;

  double Mdistance;

  MATRIX *InvSW, *SW;

  depth = mri_flash[0]->depth;
  width = mri_flash[0]->width;
  height = mri_flash[0]->height;

  SW = (MATRIX *)MatrixAlloc(nvolumes_total, nvolumes_total, MATRIX_REAL);    

  for(m1=1; m1 <= nvolumes_total; m1++){
    for(m2=m1; m2 <= nvolumes_total; m2++){
      SW->rptr[m1][m2] = 0.0; /* index starts from 1 for matrix */
    }
  }
    
  /* printf("SW matrix initialized \n"); */
  denom = 0.0;
  for(z=0; z < depth; z++)
    for(y=0; y< height; y++)
      for(x=0; x < width; x++){
	if(MRIvox(mri_mask, x, y, z) == 0) continue;
	
	if(MRIFvox(mri_mem[classID1], x, y, z) < threshold &&
	   MRIFvox(mri_mem[classID2], x, y, z) < threshold)
	  continue;

	denom +=  1.0;

	if(MRIFvox(mri_mem[classID1], x, y, z) >= threshold){
	  for(m1=0; m1 < nvolumes_total; m1++){
	    data1 = MRIFvox(mri_flash[m1], x, y, z) - LDAmean1[m1];
	    for(m2=m1; m2 < nvolumes_total; m2++){
	      data2 = MRIFvox(mri_flash[m2], x, y, z) - LDAmean1[m2];
	      SW->rptr[m1+1][m2+1] += data1*data2;
	    }
	  }
	}
	else{
	  for(m1=0; m1 < nvolumes_total; m1++){
	    data1 = MRIFvox(mri_flash[m1], x, y, z) - LDAmean2[m1];
	    for(m2=m1; m2 < nvolumes_total; m2++){
	      data2 = MRIFvox(mri_flash[m2], x, y, z) - LDAmean2[m2];
	      SW->rptr[m1+1][m2+1] += data1*data2;
	    }
	  }
	}
	
      } /* for all data points */
  
  if(denom <= 0.0)
    ErrorExit(ERROR_BADPARM, "%s: overflow in computing fuzzy covariance matrix.\n", Progname);	    
  
  for(m1=1; m1 <= nvolumes_total; m1++){
    for(m2=m1; m2 <= nvolumes_total; m2++){
      SW->rptr[m1][m2] /= denom;
      SW->rptr[m2][m1] = SW->rptr[m1][m2];
    }
    
    SW->rptr[m1][m1] += 0.00001; /* prevent SW to be singular */
  } /* for m1, m2 */


  /* Compute inverse of SW */
  InvSW = MatrixInverse(SW, NULL);

  if(InvSW == NULL){ /* inverse doesn't exist */
    ErrorExit(ERROR_BADPARM, "%s: singular fuzzy covariance matrix.\n", Progname);	          
  }
  
  /* Compute weights */
  denom = 0.0; sumw = 0.0;
  for(m1=1; m1 <= nvolumes_total; m1++){
    weights[m1-1]= 0.0;
    for(m2=1; m2 <= nvolumes_total; m2++){
      weights[m1-1] += InvSW->rptr[m1][m2] *(LDAmean1[m2-1] - LDAmean2[m2-1]);
    }
    sumw += weights[m1-1];
    denom += weights[m1-1]*weights[m1-1];
  }

  if(1){
    Mdistance = 0.0;
    for(m1=0; m1 < nvolumes_total; m1++){
      Mdistance += weights[m1]*(LDAmean1[m1] - LDAmean2[m1]);
    }
    
    printf("Mahalanobis Distance between the two classes in the original feature space is %g\n", Mdistance);

  }

  denom = sqrt(denom + 0.0000001);
  /* Normalized weights to have norm 1 */
  for(m1=1; m1 <= nvolumes_total; m1++){
    if(sumw > 0)
      weights[m1-1] /= denom;
    else
      weights[m1-1] /= -denom;
  }
  
  MatrixFree(&InvSW);
  
  MatrixFree(&SW);
  
  return;
}
  
void update_centroids(MRI **mri_flash, MRI **mri_mem, MRI **mri_lihood, MRI *mri_mask, float **centroids, int nvolumes_total, int num_classes){
  /* This step stays the same as FCM, just that the membership function is now the probability function. No, not the same! No more power of membership
   */
  int m, c, x, y, z, depth, height, width;
  double numer, denom;
  double mem, data;
  double scale =1.0;
  //  double kappa = exp(-4.5);

  depth = mri_flash[0]->depth;
  width = mri_flash[0]->width;
  height = mri_flash[0]->height;

  /* Put the voxel iteration inside makes it easier to 
   * program, but may take longer time. Otherwise, need to
   * declare numer and denom as matrices!
   */
  for(c = 0; c < num_classes; c++){
    for(m=0; m < nvolumes_total; m++){
      numer = 0; denom = 0;

      for(z=0; z < depth; z++)
	for(y=0; y< height; y++)
	  for(x=0; x < width; x++)
	    {
	      if(MRIvox(mri_mask, x, y, z) > 0){
		/* Use this "typicallity scale leads to poor results */
		// scale =  MRIFvox(mri_lihood[c], x, y, z);
		// scale = scale/(scale + kappa);
		mem = MRIFvox(mri_mem[c], x, y, z)*scale;
		data = MRIFvox(mri_flash[m], x, y, z);
		numer += mem*data;
		denom += mem;
	      }
	      
	    }
      if(denom != 0.0)
	centroids[c][m] = numer/denom;
      else{
	ErrorExit(ERROR_BADPARM, "%s: overflow in computing centroids.\n", Progname);	
      }
    }
  }


  return;
}

double distancems(MRI **mri_flash, float *centroids, MATRIX *F, double detF, int x, int y, int z){
  /* F would be the inverse of the covariance matrix of the class */
  int i, j, rows;
  double data1, data2;

  double mydistance = 0;

  rows = F->rows; /* actually the data dimensions */
  
  for(i=0; i < rows; i++){
    data1 =  MRIFvox(mri_flash[i], x, y, z) - centroids[i];
    for(j=0; j< rows; j++){
      data2 =  MRIFvox(mri_flash[j], x, y, z) - centroids[j];
      
      mydistance += data1*data2*F->rptr[i+1][j+1];
    }

  }
  
  /* No multiply by detF in the EM algorithm*/
  /* mydistance *= detF; */

  return mydistance;
}

void update_F(MATRIX **F, MRI **mri_flash, MRI **mri_mem, MRI **mri_lihood, MRI *mri_mask, float **centroids, int nvolumes_total, int num_classes){

  int m1, m2, c, x, y, z, depth, height, width;
  double denom;
  double mem, data1, data2;
  double scale = 1.0;
  //  double kappa = exp(-4.5);
  MATRIX * tmpM = 0; /*used for covariance regularization */

  depth = mri_flash[0]->depth;
  width = mri_flash[0]->width;
  height = mri_flash[0]->height;

  for(c=0; c < num_classes; c++){
    
    for(m1=1; m1 <= nvolumes_total; m1++){
      for(m2=m1; m2 <= nvolumes_total; m2++){
	F[c]->rptr[m1][m2] = 0.0; /* index starts from 1 for matrix */
      }
    }
    
    denom = 0.0;
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++){
	  if(MRIvox(mri_mask, x, y, z) == 0) continue;
	  // scale =  MRIFvox(mri_lihood[c], x, y, z);
	  // scale = scale/(scale + kappa);
	  mem = MRIFvox(mri_mem[c], x, y, z)*scale;
	  /* mem = mem*mem; */ /* here differs from FCM */
	  denom +=  mem;
	  
	  for(m1=0; m1 < nvolumes_total; m1++){
	    data1 = MRIFvox(mri_flash[m1], x, y, z) - centroids[c][m1];
	    for(m2=m1; m2 < nvolumes_total; m2++){
	      data2 = MRIFvox(mri_flash[m2], x, y, z) - centroids[c][m2];
	      F[c]->rptr[m1+1][m2+1] += data1*data2*mem;
	    }
	  }
	  
	} /* for all data points */

    if(denom <= 0.0)
      ErrorExit(ERROR_BADPARM, "%s: overflow in computing fuzzy covariance matrix.\n", Progname);	    
    
    for(m1=1; m1 <= nvolumes_total; m1++){
      for(m2=m1; m2 <= nvolumes_total; m2++){
	F[c]->rptr[m1][m2] /= denom;
	F[c]->rptr[m2][m1] = F[c]->rptr[m1][m2];
      }

      /* F[c]->rptr[m1][m1] += 1e-10; */ /* prevent F to be singular */
    } /* for m1, m2 */
    
    if(regularize){
      for(m1=1; m1 <= nvolumes_total; m1++)
	F[c]->rptr[m1][m1] += eps;  /* prevent F to be singular */
      
      tmpM = MatrixInverse(F[c], tmpM);
      if(tmpM == NULL) continue;

      /* (1-lambda)* inv(F + eps I) + labmda I */ 
      for(m1=1; m1 <= nvolumes_total; m1++){
	for(m2=m1; m2 <= nvolumes_total; m2++){
	  tmpM->rptr[m1][m2] = (1.0 - lambda)*tmpM->rptr[m1][m2];
	  tmpM->rptr[m2][m1] = tmpM->rptr[m1][m2];
	}
	tmpM->rptr[m1][m1] += lambda; 
      }

      F[c] = MatrixInverse(tmpM, F[c]);
    }
    
  } /* for(c== 0) */

  if(tmpM)
    MatrixFree(&tmpM);

  return;
}

void compute_detF(MATRIX **F, double *detF, int num_classes){

  int c, i, n;
  float tmpv;

  n = F[0]->rows;

  for(c=0; c < num_classes; c++){

    tmpv = MatrixDeterminant(F[c]);

    if(tmpv < 0.000001){ /*singular */
      for(i=1; i <= F[c]->rows; i++)
	F[c]->rptr[i][i] += 0.01; /* try to recover it */

      tmpv = MatrixDeterminant(F[c]);
    }
    
    detF[c] = tmpv;

    /* detF[c] = powf(tmpv, 1.0/n); */ /* Already took the 1-nth power */
  }

  return;
}

void compute_inverseF(MATRIX **F, int num_classes){
  MATRIX *mTmp;
  int c, row, rows, cols;

  rows =  F[0]->cols;
  cols =  F[0]->cols;

  for(c=0; c < num_classes; c++){
    mTmp = NULL;

    mTmp = MatrixInverse(F[c], mTmp);

    if(mTmp == NULL){ /* inverse doesn't exist */
      ErrorExit(ERROR_BADPARM, "%s: singular fuzzy covariance matrix.\n", Progname);	          
    }
    
    for(row=1; row <= rows; row++){
      memcpy((char *)(F[c]->rptr[row]), (char *)mTmp->rptr[row], 
	     (cols+1)*sizeof(float)) ;
    }
	
    MatrixFree(&mTmp);
    mTmp = NULL;
  }
  
  
  return;
}

void update_centroids1D(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int num_classes){

  int  c, x, y, z, depth, height, width;
  double numer, denom;
  double mem, data;

  depth = mri_flash->depth;
  width = mri_flash->width;
  height = mri_flash->height;
  
  /* Put the voxel iteration inside makes it easier to 
   * program, but may take longer time. Otherwise, need to
   * declare numer and denom as matrices!
   */
  for(c = 0; c < num_classes; c++){
    numer = 0; denom = 0;
    
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++)
	  {
	    if(MRIvox(mri_mask, x, y, z) > 0){
	      mem = MRIFvox(mri_mem[c], x, y, z);
	      data = MRIFvox(mri_flash, x, y, z);
	      /* mem = mem*mem; */ /* or powf(mem, q)  for q != 2 */
	      numer += mem*data;
	      denom += mem;
	    }
	      
	  }
    if(denom != 0.0)
      centroids1D[c] = numer/denom;
    else{
      ErrorExit(ERROR_BADPARM, "%s: overflow in computing centroids.\n", Progname);	
    }
    
  }
  

  return;
}

void MRI_EM(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int  num_classes){
  /* Simplified, assuming all class have equal size and variance 1 */
  /* 1D EM */
  int depth, width, height, x, y, z, c;
  float max_change = 100.0;
  float distance2, sum_of_distance;
  float *oldMems;
  float *sigmaI; /* variance */
  float *piI; /* class size */
  int total_num;
  

  depth = mri_flash->depth;
  width =  mri_flash->width;
  height =  mri_flash->height;

  oldMems = (float *)malloc(sizeof(float)*num_classes);
  sigmaI = (float *)malloc(sizeof(float)*num_classes);
  piI = (float *)malloc(sizeof(float)*num_classes);

  /* Initialize membership values from the given centroids. Just random */
  total_num = 0;
  for(z=0; z < depth; z++)
    for(y=0; y< height; y++)
      for(x=0; x < width; x++) {
	if(MRIvox(mri_mask, x, y, z) == 0) continue;	  
	total_num++;

	for(c=0; c < num_classes; c++){
	  MRIFvox(mri_mem[c], x, y, z) = 1.0/num_classes;
	}
      }

  printf("total_num = %d\n", total_num);
  
  for(c=0; c < num_classes; c++){
    sigmaI[c] = 1;
    piI[c] = 1.0/num_classes;
  }

  max_change = 100.0;
  while(max_change > 0.01){
    max_change = 0.0;

    /* Update membership function */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++) {
	  if(MRIvox(mri_mask, x, y, z) == 0) continue;	  
	  
	  sum_of_distance = 1e-20;
	  /* Compute distance */
	  for(c=0; c < num_classes; c++){
	    /* record old membership values */
	    oldMems[c] = MRIFvox(mri_mem[c], x, y, z);
	    
	    distance2 = MRIFvox(mri_flash, x, y, z) - centroids1D[c];

	    distance2 = piI[c]*exp(-0.5*distance2*distance2/sigmaI[c])/(sqrt(sigmaI[c]) + 1e-30);
	    
	    MRIFvox(mri_mem[c], x, y, z) = distance2;
	    
	    sum_of_distance += distance2;
	  }
	  
	  if(sum_of_distance <= 0.0)
	    ErrorExit(ERROR_BADPARM, "%s: overflow in computing membership function.\n", Progname);		    
	  
	  for(c=0; c < num_classes; c++){
	    /* borrow distance2 here */
	    distance2 = MRIFvox(mri_mem[c], x, y, z)/sum_of_distance;
	    MRIFvox(mri_mem[c], x, y, z) = distance2;
	    
	    distance2 -= oldMems[c];
	    if(distance2 < 0) distance2 = -distance2;

	    if(max_change < distance2) max_change = distance2;

	  }

	} /* end of all data points */


    printf("max_change = %g\n", max_change);

    /* Update centroids */
    update_centroids1D(mri_flash, mri_mem, mri_mask, centroids1D, num_classes);    
    printf("Centroids: ");
    for(c=0; c < num_classes; c++){
      printf(" %g,",centroids1D[c]);
    }
    printf("\n");

    /* Update class variance and priors */
    for(c=0; c < num_classes; c++){
      sigmaI[c] = 0;
      piI[c] = 0;
    }

    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++) {
	  if(MRIvox(mri_mask, x, y, z) == 0) continue;
	  for(c=0; c < num_classes; c++){
	    distance2 = MRIFvox(mri_flash, x, y, z) - centroids1D[c];
	    sigmaI[c] += distance2*distance2*MRIFvox(mri_mem[c], x, y, z);
	    piI[c] += MRIFvox(mri_mem[c], x, y, z);
	  }
	}	  
    /* Update class-priors */
    for(c=0; c < num_classes; c++){
      sigmaI[c] /= (piI[c] + 1e-30);
      piI[c] /= (float)(total_num + 1e-30);
    }

    printf("variance and class Size: \n");
    for(c=0; c < num_classes; c++){
      printf("std[%d] = %g, piI[%d]=%g\n",c, sigmaI[c], c, piI[c]);
    }
    printf("\n");
  }/* end of while-loop */

  free(oldMems);
  free(sigmaI);
  free(piI);

  return;
}


void MRI_FCM(MRI *mri_flash, MRI **mri_mem, MRI *mri_mask, float *centroids1D, int  num_classes){
  /* 1D FCM */
  int depth, width, height, x, y, z, c;
  float max_change = 100.0;
  float distance2, sum_of_distance;
  float *oldMems;

  depth = mri_flash->depth;
  width =  mri_flash->width;
  height =  mri_flash->height;

  oldMems = (float *)malloc(sizeof(float)*num_classes);

  /* Initialize membership values from the given centroids. Just random */
  for(z=0; z < depth; z++)
    for(y=0; y< height; y++)
      for(x=0; x < width; x++) {
	if(MRIvox(mri_mask, x, y, z) == 0) continue;	  
	
	for(c=0; c < num_classes; c++){
	  MRIFvox(mri_mem[c], x, y, z) = 1.0/num_classes;
	}
      }
  
  max_change = 100.0;
  while(max_change > 0.01){
    max_change = 0.0;

    /* Update membership function */
    for(z=0; z < depth; z++)
      for(y=0; y< height; y++)
	for(x=0; x < width; x++) {
	  if(MRIvox(mri_mask, x, y, z) == 0) continue;	  
	  
	  sum_of_distance = 0.0;
	  /* Compute distance */
	  for(c=0; c < num_classes; c++){
	    /* record old membership values */
	    oldMems[c] = MRIFvox(mri_mem[c], x, y, z);
	    
	    distance2 = MRIFvox(mri_flash, x, y, z) - centroids1D[c];
	    distance2 = distance2*distance2;
	    
	    if(distance2 == 0.0){
	      MRIFvox(mri_mem[c], x, y, z) = 100000000.0;
	    }
	    else{
	      MRIFvox(mri_mem[c], x, y, z) = 1.0/distance2;
	    }
	    
	    sum_of_distance += MRIFvox(mri_mem[c], x, y, z);
	  }
	  
	  if(sum_of_distance <= 0.0)
	    ErrorExit(ERROR_BADPARM, "%s: overflow in computing membership function.\n", Progname);		    
	  
	  for(c=0; c < num_classes; c++){
	    /* borrow distance2 here */
	    distance2 = MRIFvox(mri_mem[c], x, y, z)/sum_of_distance;
	    MRIFvox(mri_mem[c], x, y, z) = distance2;
	    
	    distance2 -= oldMems[c];
	    if(distance2 < 0) distance2 = -distance2;

	    if(max_change < distance2) max_change = distance2;

	  }

	} /* end of all data points */

    printf("Centroids: ");
    for(c=0; c < num_classes; c++){
      printf(" %g,",centroids1D[c]);
    }
    printf("\n");

    /* Update centroids */
    update_centroids1D(mri_flash, mri_mem, mri_mask, centroids1D, num_classes);    
    
  }/* end of while-loop */

  free(oldMems);

  return;
}

MRI *MRInormalizeXH(MRI *mri_src, MRI *mri_dst, MRI *mri_mask){
  /* Normalize the source volume to be zero mean and variance 1*/
  /* mri_dst and mri_src can be the same */

  int width, height, depth, x, y, z;
  float mean, variance, total, tmpval;
  
  if(mri_src->type != MRI_FLOAT){
    printf("Normalization is only applied for float-typed volume \n");
    mri_dst = MRIcopy(mri_src, mri_dst);
    return (mri_dst);
  }

  width = mri_src->width ;
  height = mri_src->height ;
  depth = mri_src->depth ;
  if (!mri_dst)
    mri_dst = MRIclone(mri_src, NULL) ;

  /* compute mean */
  mean = 0.0; total = 0.0;
  for (z = 0 ; z < depth ; z++)
    for (y = 0 ; y < height ; y++)
      for (x = 0 ; x < width ; x++){
	if(!mri_mask){
	  mean += MRIFvox(mri_src, x, y, z);
	  total += 1;
	}else{
	  if(MRIvox(mri_mask, x, y, z) >0){
	    mean += MRIFvox(mri_src, x, y, z);
	    total += 1;
	  }
	}
      }
  
  if(total > 0.0)
    mean = mean/total;

  /* compute variance */
  variance = 0.0;
  for (z = 0 ; z < depth ; z++)
    for (y = 0 ; y < height ; y++)
      for (x = 0 ; x < width ; x++){
	if(!mri_mask){
	  tmpval = MRIFvox(mri_src, x, y, z) - mean;
	  variance += tmpval*tmpval;
	}else{
	  if(MRIvox(mri_mask, x, y, z) >0){
	    tmpval = MRIFvox(mri_src, x, y, z) - mean;
	    variance += tmpval*tmpval;
	  }
	}
      }
  
  if(total > 0)
    variance = sqrt(variance/total);
  else
    variance = 1;

  /* normalization: invert variance first to save time */
  variance = 1.0/variance;
  for (z = 0 ; z < depth ; z++)
    for (y = 0 ; y < height ; y++)
      for (x = 0 ; x < width ; x++){
	tmpval = MRIFvox(mri_src, x, y, z) - mean;
	MRIFvox(mri_dst, x, y, z) = tmpval*variance;
      }
    
  return (mri_dst); 
}

void indexx(unsigned long n, float arr[], unsigned long indx[])
{
  unsigned long i,indxt,ir=n,itemp,j,k,l=1;
  int jstack=0,*istack;
  float a;
  int M = 7;
  int NSTACK = 50;

  istack=ivector(1,NSTACK);
  for (j=1;j<=n;j++) indx[j]=j;
  for (;;) {
    if (ir-l < M) {
      for (j=l+1;j<=ir;j++) {
	indxt=indx[j];
	a=arr[indxt];
	for (i=j-1;i>=1;i--) {
	  if (arr[indx[i]] <= a) break;
	  indx[i+1]=indx[i];
	}
	indx[i+1]=indxt;
      }
      if (jstack == 0) break;
      ir=istack[jstack--];
      l=istack[jstack--];
    } else {
      k=(l+ir) >> 1;
      SWAP(indx[k],indx[l+1]);
      if (arr[indx[l+1]] > arr[indx[ir]]) {
	SWAP(indx[l+1],indx[ir])
	  }
      if (arr[indx[l]] > arr[indx[ir]]) {
	SWAP(indx[l],indx[ir])
	  }
      if (arr[indx[l+1]] > arr[indx[l]]) {
	SWAP(indx[l+1],indx[l])
	  }
      i=l+1;
      j=ir;
      indxt=indx[l];
      a=arr[indxt];
      for (;;) {
	do i++; while (arr[indx[i]] < a);
	do j--; while (arr[indx[j]] > a);
	if (j < i) break;
	SWAP(indx[i],indx[j])
	  }
      indx[l]=indx[j];
      indx[j]=indxt;
      jstack += 2;
      if (jstack > NSTACK)
	    ErrorExit(ERROR_BADPARM, "%s: NSTACK too small in indexx.\n", Progname);		    
      if (ir-i+1 >= j-l) {
	istack[jstack]=ir;
	istack[jstack-1]=i;
	ir=j-1;
      } else {
	istack[jstack]=j-1;
	istack[jstack-1]=l;
	l=i;
      }
    }
  }
  free_ivector(istack,1,NSTACK);
  
  return;
}
