/* Copyright (C) 1999, 2000 Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**************************************************************************/

/* Here, we define the external functions callable from Guile, as defined
   by mpb.scm. */

/**************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

/* GNU Guile library header file: */
#include <guile/gh.h>

/* Header files for my eigensolver routines: */
#include "../src/config.h"
#include <mpiglue.h>
#include <check.h>
#include <blasglue.h>
#include <matrices.h>
#include <matrixio.h>
#include <eigensolver.h>
#include <maxwell.h>

/* Header file for the ctl-file (Guile) interface; automatically
   generated from mpb.scm */
#include <ctl-io.h>

/* Routines from libctl/utils/geom.c: */
#include <ctlgeom.h>

/* shared declarations for the files in mpb-ctl: */
#include "mpb.h"

/* this integer flag is defined by main.c from libctl, and is
   set when the user runs the program with --verbose */
extern int verbose;

/**************************************************************************/

/* a couple of utilities to convert libctl data types to the data
   types of the eigensolver & maxwell routines: */

static void vector3_to_arr(real arr[3], vector3 v)
{
     arr[0] = v.x;
     arr[1] = v.y;
     arr[2] = v.z;
}

static void matrix3x3_to_arr(real arr[3][3], matrix3x3 m)
{
     vector3_to_arr(arr[0], m.c0);
     vector3_to_arr(arr[1], m.c1);
     vector3_to_arr(arr[2], m.c2);
}

/**************************************************************************/

#define NWORK 3
#define NUM_FFT_BANDS 20 /* max number of bands to FFT at a time */

/* global variables for retaining data about the eigenvectors between
   calls from Guile: */

static maxwell_data *mdata = NULL;
static maxwell_target_data *mtdata = NULL;
static evectmatrix H, W[NWORK], Hblock;

static scalar_complex *curfield = NULL;
static int curfield_band;
static char curfield_type = '-';

/* R[i]/G[i] are lattice/reciprocal-lattice vectors */
static real R[3][3], G[3][3];
static matrix3x3 Rm, Gm; /* same thing, but matrix3x3 */

/* index of current kpoint, for labeling output */
static int kpoint_index = 0;

#define USE_GEOMETRY_TREE 1
geom_box_tree geometry_tree = NULL; /* recursive tree of geometry objects
				       for fast searching */

/**************************************************************************/

typedef struct {
     maxwell_dielectric_function eps_file_func;
     void *eps_file_func_data;
} epsilon_func_data;

/* Given a position r in the basis of the lattice vectors, return the
   corresponding dielectric tensor and its inverse.  Should be
   called from within init_params (or after init_params), so that the
   geometry input variables will have been read in (for use in libgeom).

   This function is passed to set_maxwell_dielectric to initialize
   the dielectric tensor array for eigenvector calculations. */

static void epsilon_func(symmetric_matrix *eps, symmetric_matrix *eps_inv,
			 real r[3], void *edata)
{
     epsilon_func_data *d = (epsilon_func_data *) edata;
     material_type material;
     vector3 p;
     boolean inobject;

     /* p needs to be in the lattice *unit* vector basis, while r is
	in the lattice vector basis.  Also, shift origin to the center
        of the grid. */
     p.x = (r[0] - 0.5) * geometry_lattice.size.x;
     p.y = dimensions <= 1 ? 0 : (r[1] - 0.5) * geometry_lattice.size.y;
     p.z = dimensions <= 2 ? 0 : (r[2] - 0.5) * geometry_lattice.size.z;

     /* call search routine from libctl/utils/libgeom/geom.c: */
#if USE_GEOMETRY_TREE
     material = material_of_point_in_tree_inobject(p, geometry_tree, 
						   &inobject);
#else
     material = material_of_point_inobject(p, &inobject);
#endif

     /* if we aren't in any geometric object and we have an epsilon
	file, use that. */
     if (!inobject && d->eps_file_func) {
	  d->eps_file_func(eps, eps_inv, r, d->eps_file_func_data);
     }
     else {
	  if (material.which_subclass == MATERIAL_TYPE_SELF)
	       material = default_material;
	  switch (material.which_subclass) {
	      case DIELECTRIC:
	      {
		   real eps_val = material.subclass.dielectric_data->epsilon;
		   eps->m00 = eps->m11 = eps->m22 = eps_val;
		   eps->m01 = eps->m02 = eps->m12 = 0.0;
		   eps_inv->m00 = eps_inv->m11 = eps_inv->m22 = 1.0 / eps_val;
		   eps_inv->m01 = eps_inv->m02 = eps_inv->m12 = 0.0;
		   break;
	      }
	      case DIELECTRIC_ANISOTROPIC:
	      {
		   dielectric_anisotropic *d =
			material.subclass.dielectric_anisotropic_data;
		   eps->m00 = d->epsilon_diag.x;
		   eps->m11 = d->epsilon_diag.y;
		   eps->m22 = d->epsilon_diag.z;
		   eps->m01 = d->epsilon_offdiag.x;
		   eps->m02 = d->epsilon_offdiag.y;
		   eps->m12 = d->epsilon_offdiag.z;
		   maxwell_sym_matrix_invert(eps_inv, eps);
		   break;
	      }
	      case MATERIAL_TYPE_SELF:
		   CHECK(0, "invalid use of material-type");
		   break;
	  }
     }
}

/**************************************************************************/

/* initialize the field to random numbers; should only be called
   after init-params.  (Guile-callable.) */
void randomize_fields(void)
{
     int i;

     if (!mdata) {
	  fprintf(stderr,
		  "init-params must be called before randomize-fields!\n");
	  return;
     }
     printf("Initializing fields to random numbers...\n");
     for (i = 0; i < H.n * H.p; ++i) {
	  ASSIGN_SCALAR(H.data[i], rand() * 1.0 / RAND_MAX,
			rand() * 1.0 / RAND_MAX);
     }
}

/**************************************************************************/

/* Set the current polarization to solve for. (init-params should have
   already been called.  (Guile-callable; see mpb.scm.) 

   p = 0 means NO_POLARIZATION
   p = 1 means TE_POLARIZATION
   p = 2 means TM_POLARIZATION
   p = 3 means EVEN_Z_POLARIZATION
   p = 4 means ODD_Z_POLARIZATION
   p = -1 means the polarization of the previous call, 
       or NO_POLARIZATION if this is the first call */

void set_polarization(int p)
{
     static int last_p = -2;  /* initialize to some non-value */

     if (!mdata) {
	  fprintf(stderr,
		  "init-params must be called before set-polarization!\n");
	  return;
     }

     if (p == -1)
	  p = last_p < 0 ? 0 : last_p;

     switch (p) {
	 case 0:
	      printf("Solving for non-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, NO_POLARIZATION);
	      break;
	 case 1:
	      printf("Solving for TE-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, TE_POLARIZATION);
	      break;
	 case 2:
	      printf("Solving for TM-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, TM_POLARIZATION);
	      break;
	 case 3:
	      printf("Solving for bands even about z=0.\n");
	      set_maxwell_data_polarization(mdata, EVEN_Z_POLARIZATION);
	      break;
	 case 4:
	      printf("Solving for bands odd about z=0.\n");
	      set_maxwell_data_polarization(mdata, ODD_Z_POLARIZATION);
	      break;
	 default:
	      fprintf(stderr, "Unknown polarization type!\n");
	      return;
     }

     last_p = p;
     kpoint_index = 0;  /* reset index */
}

static char polarization_strings[5][10] = {
     "",      /* NO_POLARIZATION */
     "te",    /* TE_POLARIZATION */
     "tm",    /* TM_POLARIZATION */
     "even",  /* EVEN_Z_POLARIZATION */
     "odd"    /* ODD_Z_POLARIZATION */
};

/**************************************************************************/

/* Guile-callable function: init-params, which initializes any data
   that we need for the eigenvalue calculation.  When this function
   is called, the input variables (the geometry, etcetera) have already
   been read into the global variables defined in ctl-io.h.  
   
   p is the polarization to use for the coming calculation, although
   this can be changed by calling set-polarization.  p is interpreted
   in the same way as for set-polarization.

   If reset_fields is false, then any fields from a previous run are
   retained if they are of the same dimensions.  Otherwise, new
   fields are allocated and initialized to random numbers. */
void init_params(int p, boolean reset_fields)
{
     int i, local_N, N_start, alloc_N;
     int nx, ny, nz;
     int mesh[3];
     int have_old_fields = 0;
     int tree_depth, tree_nobjects;
     int block_size;
     
     /* Output a bunch of stuff so that the user can see what we're
	doing and what we've read in. */
     
     printf("init-params: initializing eigensolver data\n");
#ifndef SCALAR_COMPLEX
     printf("  -- assuming INVERSION SYMMETRY in the geometry.\n");
#endif
     
     printf("Computing %d bands with %e tolerance.\n", num_bands, tolerance);
     if (target_freq != 0.0)
	  printf("Target frequency is %g\n", target_freq);
     
     nx = grid_size.x;
     ny = grid_size.y;
     nz = grid_size.z;

     if (eigensolver_block_size != 0 && eigensolver_block_size < num_bands) {
	  block_size = eigensolver_block_size;
	  if (block_size < 0) {
	       /* Guess a block_size near -block_size, chosen so that
		  all blocks are nearly equal in size: */
	       block_size = (num_bands - block_size - 1) / (-block_size);
	       block_size = (num_bands + block_size - 1) / block_size;
	  }
	  printf("Solving for %d bands at a time.\n", block_size);
     }
     else
	  block_size = num_bands;

     {
	  int true_rank = nz > 1 ? 3 : (ny > 1 ? 2 : 1);
	  if (true_rank < dimensions)
	       dimensions = true_rank;
	  else if (true_rank > dimensions) {
	       fprintf(stderr, 
		       "WARNING: rank of grid is > dimensions.\n"
		       "         setting extra grid dims. to 1.\n");
	       /* force extra dims to be 1: */
	       if (dimensions <= 2)
		    nz = 1;
	       if (dimensions <= 1)
		    ny = 1;
	  }
     }

     printf("Working in %d dimensions.\n", dimensions);

     printf("Grid size is %d x %d x %d.\n", nx, ny, nz);

     printf("Mesh size is %d.\n", mesh_size);
     mesh[0] = mesh_size;
     mesh[1] = (dimensions > 1) ? mesh_size : 1;
     mesh[2] = (dimensions > 2) ? mesh_size : 1;

     Rm.c0 = vector3_scale(geometry_lattice.size.x, geometry_lattice.basis.c0);
     Rm.c1 = vector3_scale(geometry_lattice.size.y, geometry_lattice.basis.c1);
     Rm.c2 = vector3_scale(geometry_lattice.size.z, geometry_lattice.basis.c2);
     printf("Lattice vectors:\n");
     printf("     (%g, %g, %g)\n", Rm.c0.x, Rm.c0.y, Rm.c0.z);  
     printf("     (%g, %g, %g)\n", Rm.c1.x, Rm.c1.y, Rm.c1.z);
     printf("     (%g, %g, %g)\n", Rm.c2.x, Rm.c2.y, Rm.c2.z);
  
     Gm = matrix3x3_inverse(matrix3x3_transpose(Rm));
     printf("Reciprocal lattice vectors (/ 2 pi):\n");
     printf("     (%g, %g, %g)\n", Gm.c0.x, Gm.c0.y, Gm.c0.z);  
     printf("     (%g, %g, %g)\n", Gm.c1.x, Gm.c1.y, Gm.c1.z);
     printf("     (%g, %g, %g)\n", Gm.c2.x, Gm.c2.y, Gm.c2.z);
     
     matrix3x3_to_arr(R, Rm);
     matrix3x3_to_arr(G, Gm);

     /* we must do this to correct for a non-orthogonal lattice basis: */
     geom_fix_objects();

     printf("Geometric objects:\n");
     for (i = 0; i < geometry.num_items; ++i) {
	  display_geometric_object_info(5, geometry.items[i]);

	  if (geometry.items[i].material.which_subclass == DIELECTRIC)
	       printf("%*sdielectric constant epsilon = %g\n", 5 + 5, "",
		      geometry.items[i].material.
		      subclass.dielectric_data->epsilon);
     }

     destroy_geom_box_tree(geometry_tree);  /* destroy any tree from
					       previous runs */
     geometry_tree =  create_geom_box_tree();
     if (verbose) {
	  printf("Geometry object bounding box tree:\n");
	  display_geom_box_tree(5, geometry_tree);
     }
     geom_box_tree_stats(geometry_tree, &tree_depth, &tree_nobjects);
     printf("Geometric object tree has depth %d and %d object nodes"
	    " (vs. %d actual objects)\n",
	    tree_depth, tree_nobjects, geometry.num_items);

     printf("%d k-points:\n", k_points.num_items);
     for (i = 0; i < k_points.num_items; ++i)
	  printf("     (%g,%g,%g)\n", k_points.items[i].x,
		 k_points.items[i].y, k_points.items[i].z);
     
     if (mdata) {  /* need to clean up from previous init_params call */
	  if (nx == mdata->nx && ny == mdata->ny && nz == mdata->nz &&
	      block_size == Hblock.alloc_p && num_bands == H.p)
	       have_old_fields = 1; /* don't need to reallocate */
	  else {
	       destroy_evectmatrix(H);
	       for (i = 0; i < NWORK; ++i)
		    destroy_evectmatrix(W[i]);
	       if (Hblock.data != H.data)
		    destroy_evectmatrix(Hblock);
	  }
	  destroy_maxwell_target_data(mtdata); mtdata = NULL;
	  destroy_maxwell_data(mdata); mdata = NULL;
	  curfield = NULL;
     }
     else
	  srand(time(NULL)); /* init random seed for field initialization */
   
     if (deterministicp) {  /* check input variable "deterministic?" */
	  /* seed should be the same for each run, although
	     it should be different for each process: */
	  int rank;
	  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	  srand(314159 * (rank + 1));
     }

     printf("Creating Maxwell data...\n");
     mdata = create_maxwell_data(nx, ny, nz, &local_N, &N_start, &alloc_N,
                                 block_size, NUM_FFT_BANDS);
     CHECK(mdata, "NULL mdata");

     printf("Initializing dielectric function...\n");
     {
	  epsilon_func_data d;
	  get_epsilon_file_func(epsilon_input_file,
				&d.eps_file_func, &d.eps_file_func_data);
	  set_maxwell_dielectric(mdata, mesh, R, G, epsilon_func, &d);
	  destroy_epsilon_file_func_data(d.eps_file_func_data);
     }

     if (target_freq != 0.0)
	  mtdata = create_maxwell_target_data(mdata, target_freq);
     else
	  mtdata = NULL;

     if (!have_old_fields) {
	  printf("Allocating fields...\n");
	  H = create_evectmatrix(nx * ny * nz, 2, num_bands,
				 local_N, N_start, alloc_N);
	  for (i = 0; i < NWORK; ++i)
	       W[i] = create_evectmatrix(nx * ny * nz, 2, block_size,
					 local_N, N_start, alloc_N);
	  if (block_size < num_bands)
	       Hblock = create_evectmatrix(nx * ny * nz, 2, block_size,
					   local_N, N_start, alloc_N);
	  else
	       Hblock = H;
     }

     set_polarization(p);
     if (!have_old_fields || reset_fields)
	  randomize_fields();
}

/**************************************************************************/

/* When we are solving for a few bands at a time, we solve for the
   upper bands by "deflation"--by continually orthogonalizing them
   against the already-computed lower bands.  (This constraint
   commutes with the eigen-operator, of course, so all is well.) */

typedef struct {
     evectmatrix Y;  /* the vectors to orthogonalize against; Y must
			itself be normalized (Yt Y = 1) */
     int p;  /* the number of columns of Y to orthogonalize against */
     scalar *S;  /* a matrix for storing the dot products; should have
		    at least p * X.p elements (see below for X) */
} deflation_data;

static void deflation_constraint(evectmatrix X, void *data)
{
     deflation_data *d = (deflation_data *) data;

     CHECK(X.n == d->Y.n && d->Y.p >= d->p, "invalid dimensions");

     /* (Sigh...call the BLAS functions directly since we are not
	using all the columns of Y...evectmatrix is not set up for
	this case.) */

     /* compute S = Xt Y (i.e. all the dot products): */
     blasglue_gemm('C', 'N', X.p, d->p, X.n,
		   1.0, X.data, X.p, d->Y.data, d->Y.p, 0.0, d->S, d->p);
     MPI_Allreduce(d->S, d->S, d->p * X.p * SCALAR_NUMVALS,
		   SCALAR_MPI_TYPE, MPI_SUM, MPI_COMM_WORLD);

     /* compute X = X - Y*St = (1 - Y Yt) X */
     blasglue_gemm('N', 'C', X.n, X.p, d->p,
		   -1.0, d->Y.data, d->Y.p, d->S, d->p,
		   1.0, X.data, X.p);
}

/**************************************************************************/

/* Solve for the bands at a given k point.
   Must only be called after init_params! */
void solve_kpoint(vector3 kvector)
{
     int i, total_iters = 0, ib, ib0;
     real *eigvals;
     real k[3];
     int flags;
     deflation_data deflation;

     printf("solve_kpoint (%g,%g,%g):\n",
	    kvector.x, kvector.y, kvector.z);

     curfield_type = '-'; /* reset curfield, invalidating stored fields */

     if (num_bands == 0) {
	  printf("  num-bands is zero, not solving for any bands\n");
	  return;
     }

     if (!mdata) {
	  fprintf(stderr, "init-params must be called before solve-kpoint!\n");
	  return;
     }

     /* if this is the first k point, print out a header line for
	for the frequency grep data: */
     if (!kpoint_index) {
	  printf("%sfreqs:, k index, kx, ky, kz, kmag/2pi",
		 polarization_strings[mdata->polarization]);
	  for (i = 0; i < num_bands; ++i)
	       printf(", %s%sband %d", 
		      polarization_strings[mdata->polarization],
		      mdata->polarization == NO_POLARIZATION ? "" : " ",
		      i + 1);
	  printf("\n");
     }

     vector3_to_arr(k, kvector);
     update_maxwell_data_k(mdata, k, G[0], G[1], G[2]);

     CHK_MALLOC(eigvals, real, num_bands);

     flags = eigensolver_flags; /* ctl file input variable */
     if (verbose)
	  flags |= EIGS_VERBOSE;

     /* constant (zero frequency) bands at k=0 are handled specially,
        so remove them from the solutions for the eigensolver: */
     if (mdata->zero_k && !mtdata) {
	  int in, ip;
	  ib0 = maxwell_zero_k_num_const_bands(H, mdata);
	  for (in = 0; in < H.n; ++in)
	       for (ip = 0; ip < H.p - ib0; ++ip)
		    H.data[in * H.p + ip] = H.data[in * H.p + ip + ib0];
	  evectmatrix_resize(&H, H.p - ib0, 1);
     }
     else
	  ib0 = 0; /* solve for all bands */

     /* Set up deflation data: */
     if (H.data != Hblock.data) {
	  deflation.Y = H;
	  deflation.p = 0;
	  CHK_MALLOC(deflation.S, scalar, H.p * Hblock.p);
     }

     for (ib = ib0; ib < num_bands; ib += Hblock.alloc_p) {
	  evectconstraint_chain *constraints;
	  int num_iters;

	  /* don't solve for too many bands if the block size doesn't divide
	     the number of bands: */
	  if (ib + mdata->num_bands > num_bands) {
	       maxwell_set_num_bands(mdata, num_bands - ib);
	       for (i = 0; i < NWORK; ++i)
		    evectmatrix_resize(&W[i], num_bands - ib, 0);
	       evectmatrix_resize(&Hblock, num_bands - ib, 0);
	  }

	  printf("Solving for bands %d to %d...\n", ib + 1, ib + Hblock.p);

	  constraints = NULL;
	  constraints = evect_add_constraint(constraints,
					     maxwell_constraint,
					     (void *) mdata);

	  if (mdata->zero_k)
	       constraints = evect_add_constraint(constraints,
						  maxwell_zero_k_constraint,
						  (void *) mdata);

	  if (Hblock.data != H.data) {  /* initialize fields of block from H */
	       int in, ip;
	       for (in = 0; in < Hblock.n; ++in)
		    for (ip = 0; ip < Hblock.p; ++ip)
			 Hblock.data[in * Hblock.p + ip] =
			      H.data[in * H.p + ip + (ib-ib0)];
	       deflation.p = ib-ib0;
	       if (deflation.p > 0)
		    constraints = evect_add_constraint(constraints,
						       deflation_constraint,
						       &deflation);
	  }

	  if (mtdata) {  /* solving for bands near a target frequency */
	       eigensolver(Hblock, eigvals + ib,
			   maxwell_target_operator, (void *) mtdata,
			   simple_preconditionerp ? 
			   maxwell_target_preconditioner :
			   maxwell_target_preconditioner2,
			   (void *) mtdata,
			   evectconstraint_chain_func, (void *) constraints,
			   W, NWORK, tolerance, &num_iters, flags);
	       /* now, diagonalize the real Maxwell operator in the
		  solution subspace to get the true eigenvalues and
		  eigenvectors: */
	       CHECK(NWORK >= 2, "not enough workspace");
	       eigensolver_get_eigenvals(Hblock, eigvals + ib,
					 maxwell_operator,mdata, W[0],W[1]);
	  }
	  else {
	       eigensolver(Hblock, eigvals + ib,
			   maxwell_operator, (void *) mdata,
			   simple_preconditionerp ?
			   maxwell_preconditioner :
			   maxwell_preconditioner2,
			   (void *) mdata,
			   evectconstraint_chain_func, (void *) constraints,
			   W, NWORK, tolerance, &num_iters, flags);
	  }
	  
	  if (Hblock.data != H.data) {  /* save solutions of current block */
	       int in, ip;
	       for (in = 0; in < Hblock.n; ++in)
		    for (ip = 0; ip < Hblock.p; ++ip)
			 H.data[in * H.p + ip + (ib-ib0)] =
			      Hblock.data[in * Hblock.p + ip];
	  }

	  evect_destroy_constraints(constraints);
	  
	  printf("Finished solving for bands %d to %d after %d iterations.\n",
		 ib + 1, ib + Hblock.p, num_iters);
	  total_iters += num_iters * Hblock.p;
     }

     if (num_bands - ib0 > Hblock.alloc_p)
	  printf("Finished k-point with %g mean iterations per band.\n",
		 total_iters * 1.0 / num_bands);

     /* Manually put in constant (zero-frequency) solutions for k=0: */
     if (mdata->zero_k && !mtdata) {
	  int in, ip;
	  evectmatrix_resize(&H, H.alloc_p, 1);
	  for (in = 0; in < H.n; ++in)
	       for (ip = H.p - ib0 - 1; ip >= 0; --ip)
		    H.data[in * H.p + ip + ib0] = H.data[in * H.p + ip];
	  maxwell_zero_k_set_const_bands(H, mdata);
	  for (ib = 0; ib < ib0; ++ib)
	       eigvals[ib] = 0;
     }

     /* Reset scratch matrix sizes: */
     evectmatrix_resize(&Hblock, Hblock.alloc_p, 0);
     for (i = 0; i < NWORK; ++i)
	  evectmatrix_resize(&W[i], W[i].alloc_p, 0);
     maxwell_set_num_bands(mdata, Hblock.alloc_p);

     /* Destroy deflation data: */
     if (H.data != Hblock.data) {
	  free(deflation.S);
     }

     if (num_write_output_vars > 1) {
	  /* clean up from prev. call */
	  free(freqs.items);
	  free(z_parity.items);
     }

     iterations = total_iters; /* iterations output variable */

     /* create freqs array for storing frequencies in a Guile list */
     freqs.num_items = num_bands;
     CHK_MALLOC(freqs.items, number, freqs.num_items);
     
     printf("%sfreqs:, %d, %g, %g, %g, %g",
	    polarization_strings[mdata->polarization],
	    ++kpoint_index, k[0], k[1], k[2],
	    vector3_norm(matrix3x3_vector3_mult(Gm, kvector)));
     for (i = 0; i < num_bands; ++i) {
	  freqs.items[i] = sqrt(eigvals[i]);
	  printf(", %g", freqs.items[i]);
     }
     printf("\n");

     z_parity.num_items = num_bands;
     z_parity.items = maxwell_zparity(H, mdata);
     printf("%szparity:, %d", polarization_strings[mdata->polarization],
	    kpoint_index);
     for (i = 0; i < num_bands; ++i)
	  printf(", %g", z_parity.items[i]);
     printf("\n");

     free(eigvals);
     curfield = NULL;
}

/**************************************************************************/

/* Compute the group velocity dw/dk in the given direction d (where
   the length of d is ignored).  d is in the reciprocal lattice basis.
   Should only be called after solve_kpoint.  Returns a list of the
   group velocities, one for each band, in units of c. */
number_list compute_group_velocity_component(vector3 d)
{
     number_list group_v;
     real u[3];
     int i;

     group_v.num_items = 0;  group_v.items = (number *) NULL;

     if (!mdata) {
	  fprintf(stderr, "init-params must be called first!\n");
	  return group_v;
     }
     if (!kpoint_index) {
	  fprintf(stderr, "solve-kpoint must be called first!\n");
	  return group_v;
     }

     /* convert d to unit vector in Cartesian coords: */
     d = unit_vector3(matrix3x3_vector3_mult(Gm, d));
     u[0] = d.x; u[1] = d.y; u[2] = d.z;

     group_v.num_items = num_bands;
     CHK_MALLOC(group_v.items, number, group_v.num_items);
     
     /* now, compute group_v.items = diag Re <H| curl 1/eps i u x |H>: */
     maxwell_ucross_op(H, W[0], mdata, u);
     evectmatrix_XtY_diag_real(H, W[0], group_v.items);

     /* The group velocity is given by:

	grad_k(omega)*d = grad_k(omega^2)*d / 2*omega
	   = grad_k(<H|maxwell_op|H>)*d / 2*omega
	   = Re <H| curl 1/eps i u x |H> / omega
        
        Note that our k is in units of 2*Pi/a, and omega is in
        units of 2*Pi*c/a, so the result will be in units of c. */
     for (i = 0; i < num_bands; ++i)
	  group_v.items[i] = group_v.items[i] / freqs.items[i];
     
     return group_v;
}

/**************************************************************************/

/* The following routines take the eigenvectors computed by solve-kpoint
   and compute the field (D, H, or E) in position space for one of the bands.
   This field is stored in the global curfield (actually an alias for
   mdata->fft_data, since the latter is unused and big enough).  This
   field can then be manipulated with subsequent "*-field-*" functions
   below.  You can also get the scalar field, epsilon.

   All of these functions are designed to be called by the user
   via Guile. */

void get_dfield(int which_band)
{
     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-dfield!\n");
	  return;
     }
     if (!kpoint_index) {
	  fprintf(stderr, "solve-kpoint must be called before get-dfield!\n");
	  return;
     }
     if (which_band < 1 || which_band > H.p) {
	  fprintf(stderr, "must have 1 <= band index <= num_bands (%d)\n",H.p);
	  return;
     }

     curfield = (scalar_complex *) mdata->fft_data;
     curfield_band = which_band;
     curfield_type = 'd';
     maxwell_compute_d_from_H(mdata, H, curfield, which_band - 1, 1);
}

void get_hfield(int which_band)
{
     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-hfield!\n");
	  return;
     }
     if (!kpoint_index) {
	  fprintf(stderr, "solve-kpoint must be called before get-hfield!\n");
	  return;
     }
     if (which_band < 1 || which_band > H.p) {
	  fprintf(stderr, "must have 1 <= band index <= num_bands (%d)\n",H.p);
	  return;
     }

     curfield = (scalar_complex *) mdata->fft_data;
     curfield_band = which_band;
     curfield_type = 'h';
     maxwell_compute_h_from_H(mdata, H, curfield, which_band - 1, 1);
}

void get_efield_from_dfield(void)
{
     if (!curfield || curfield_type != 'd') {
	  fprintf(stderr, "get-dfield must be called before "
		  "get-efield-from-dfield!\n");
	  return;
     }
     CHECK(mdata, "unexpected NULL mdata");
     maxwell_compute_e_from_d(mdata, curfield, 1);
     curfield_type = 'e';
}

void get_efield(int which_band)
{
     get_dfield(which_band);
     get_efield_from_dfield();
}

/* get the dielectric function, and compute some statistics */
void get_epsilon(void)
{
     int i, N, last_dim, last_dim_stored;
     real *epsilon;
     real eps_mean = 0, eps_inv_mean = 0, eps_high = -1e20, eps_low = 1e20;
     int fill_count = 0;

     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-epsilon!\n");
	  return;
     }

     curfield = (scalar_complex *) mdata->fft_data;
     epsilon = (real *) curfield;
     curfield_band = 0;
     curfield_type = 'n';

     /* get epsilon.  Recall that we actually have an inverse
	dielectric tensor at each point; define an average index by
	the inverse of the average eigenvalue of the 1/eps tensor.
	i.e. 3/(trace 1/eps). */

     N = mdata->fft_output_size;
     last_dim = mdata->last_dim;
     last_dim_stored =
	  mdata->last_dim_size / (sizeof(scalar_complex)/sizeof(scalar));
     for (i = 0; i < N; ++i) {
          epsilon[i] = 3.0 / (mdata->eps_inv[i].m00 +
                              mdata->eps_inv[i].m11 +
                              mdata->eps_inv[i].m22);
	  if (epsilon[i] < eps_low)
	       eps_low = epsilon[i];
	  if (epsilon[i] > eps_high)
	       eps_high = epsilon[i];
	  eps_mean += epsilon[i];
	  eps_inv_mean += 1/epsilon[i];
	  if (epsilon[i] > 1.0001)
	       ++fill_count;
#ifndef SCALAR_COMPLEX
	  /* most points need to be counted twice, by rfftw output symmetry: */
	  {
	       int last_index = i % last_dim_stored;
	       if (last_index != 0 && 2*last_index != last_dim) {
		    eps_mean += epsilon[i];
		    eps_inv_mean += 1/epsilon[i];
		    if (epsilon[i] > 1.0001)
			 ++fill_count;
	       }
	  }
#endif
     }

     MPI_Allreduce(&eps_mean, &eps_mean, 1, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     MPI_Allreduce(&eps_inv_mean, &eps_inv_mean, 1, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     MPI_Allreduce(&eps_low, &eps_low, 1, SCALAR_MPI_TYPE,
                   MPI_MIN, MPI_COMM_WORLD);
     MPI_Allreduce(&eps_high, &eps_high, 1, SCALAR_MPI_TYPE,
                   MPI_MAX, MPI_COMM_WORLD);
     MPI_Allreduce(&fill_count, &fill_count, 1, MPI_INT,
                   MPI_SUM, MPI_COMM_WORLD);
     N = mdata->nx * mdata->ny * mdata->nz;
     eps_mean /= N;
     eps_inv_mean = N/eps_inv_mean;

     printf("epsilon: %g-%g, mean %g, harm. mean %g, "
	    "%g%% > 1, %g%% \"fill\"\n",
	    eps_low, eps_high, eps_mean, eps_inv_mean,
	    (100.0 * fill_count) / N, 
	    100.0 * (eps_mean-eps_low) / (eps_high-eps_low));
}

/**************************************************************************/

/* Replace curfield (either d or h) with the scalar energy density function,
   normalized to one.  While we're at it, compute some statistics about
   the relative strength of different field components.  Also return
   the integral of the energy density, which we used to normalize it,
   in case the user needs the unnormalized version. */
number compute_field_energy(void)
{
     int i, N, last_dim, last_dim_stored;
     real comp_sum[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
     real energy_sum = 0.0, normalization;
     real *energy_density = (real *) curfield;

     if (!curfield || !strchr("dh", curfield_type)) {
	  fprintf(stderr, "The D or H field must be loaded first.\n");
	  return 0;
     }

     N = mdata->fft_output_size;
     last_dim = mdata->last_dim;
     last_dim_stored =
	  mdata->last_dim_size / (sizeof(scalar_complex)/sizeof(scalar));
     for (i = 0; i < N; ++i) {
	  scalar_complex field[3];
	  real
	       comp_sqr0,comp_sqr1,comp_sqr2,comp_sqr3,comp_sqr4,comp_sqr5;

	  /* energy is either |curfield|^2 or |curfield|^2 / epsilon,
	     depending upon whether it is H or D. */
	  if (curfield_type == 'h') {
	       field[0] =   curfield[3*i];
	       field[1] = curfield[3*i+1];
	       field[2] = curfield[3*i+2];
	  }
	  else
	       assign_symmatrix_vector(field, mdata->eps_inv[i], curfield+3*i);

	  comp_sum[0] += comp_sqr0 = field[0].re *   curfield[3*i].re;
	  comp_sum[1] += comp_sqr1 = field[0].im *   curfield[3*i].im;
	  comp_sum[2] += comp_sqr2 = field[1].re * curfield[3*i+1].re;
	  comp_sum[3] += comp_sqr3 = field[1].im * curfield[3*i+1].im;
	  comp_sum[4] += comp_sqr4 = field[2].re * curfield[3*i+2].re;
	  comp_sum[5] += comp_sqr5 = field[2].im * curfield[3*i+2].im;

	  /* Note: here, we write to energy_density[i]; this is
	     safe, even though energy_density is aliased to curfield,
	     since energy_density[i] is guaranteed to come at or before
	     curfield[i] (which we are now done with). */

	  energy_sum += energy_density[i] = 
	       comp_sqr0+comp_sqr1+comp_sqr2+comp_sqr3+comp_sqr4+comp_sqr5;
#ifndef SCALAR_COMPLEX
	  /* most points need to be counted twice, by rfftw output symmetry: */
	  {
	       int last_index = i % last_dim_stored;
	       if (last_index != 0 && 2*last_index != last_dim) {
		    energy_sum += energy_density[i];
		    comp_sum[0] += comp_sqr0;
		    comp_sum[1] += comp_sqr1;
		    comp_sum[2] += comp_sqr2;
		    comp_sum[3] += comp_sqr3;
		    comp_sum[4] += comp_sqr4;
		    comp_sum[5] += comp_sqr5;
	       }
	  }
#endif
     }

     MPI_Allreduce(&energy_sum, &energy_sum, 1, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     MPI_Allreduce(comp_sum, comp_sum, 6, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);

     normalization = 1.0 / (energy_sum == 0 ? 1 : energy_sum);
     for (i = 0; i < N; ++i)
	  energy_density[i] *= normalization;

     printf("%c-energy-components:, %d, %d",
	    curfield_type, kpoint_index, curfield_band);
     for (i = 0; i < 6; ++i) {
	  comp_sum[i] *= normalization;
	  if (i % 2 == 1)
	       printf(", %g", comp_sum[i] + comp_sum[i-1]);
     }
     printf("\n");

     /* remember that we now have energy density; denoted by capital D/H */
     curfield_type = toupper(curfield_type);

     /* Return the total energy.  Divide by N to account for the
	scaling of the Fourier transform, so that the integral is
	consistent with the integral in frequency-domain. */
     return (energy_sum/N);
}

/**************************************************************************/

/* Fix the phase of the current field (e/h/d) to a canonical value.
   Also changes the phase of the corresponding eigenvector by the
   same amount, so that future calculations will have a consistent
   phase.

   The following procedure is used, derived from a suggestion by Doug
   Allan of Corning: First, choose the phase to maximize the sum of
   the squares of the real parts of the components.  This doesn't fix
   the overall sign, though.  That is done (after incorporating the
   above phase) by: (1) find the largest absolute value of the real
   part, (2) find the point with the greatest spatial array index that
   has |real part| at least half of the largest value, and (3) make
   that point positive.

   In the case of inversion symmetry, on the other hand, the overall phase
   is already fixed, to within a sign, by the choice to make the Fourier
   transform purely real.  So, in that case we simply pick a sign, in
   a manner similar to (2) and (3) above. */
void fix_field_phase(void)
{
     int i, N;
     real sq_sum[2] = {0,0}, maxabs = 0.0;
     int maxabs_index = 0, maxabs_sign = 1;
     double theta;
     scalar phase;

     if (!curfield || !strchr("dhe", curfield_type)) {
          fprintf(stderr, "The D, H, or E field must be loaded first.\n");
          return;
     }
     N = mdata->fft_output_size * 3;

#ifdef SCALAR_COMPLEX
     /* Compute the phase that maximizes the sum of the squares of
	the real parts of the components.  Equivalently, maximize
	the real part of the sum of the squares. */
     for (i = 0; i < N; ++i) {
	  real a,b;
	  a = curfield[i].re; b = curfield[i].im;
	  sq_sum[0] += a*a - b*b;
	  sq_sum[1] += 2*a*b;
     }
     MPI_Allreduce(sq_sum, sq_sum, 2, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     /* compute the phase = exp(i*theta) maximizing the real part of
	the sum of the squares.  i.e., maximize:
	    cos(2*theta)*sq_sum[0] - sin(2*theta)*sq_sum[1] */
     theta = 0.5 * atan2(-sq_sum[1], sq_sum[0]);
     phase.re = cos(theta);
     phase.im = sin(theta);
#else /* ! SCALAR_COMPLEX */
     phase = 1;
#endif /* ! SCALAR_COMPLEX */

     /* Next, fix the overall sign.  We do this by first computing the
	maximum |real part| of the jmax component (after multiplying
	by phase), and then finding the last spatial index at which
	|real part| is at least half of this value.  The sign is then
	chosen to make the real part positive at that point. 

        (Note that we can't just make the point of maximum |real part|
         positive, as that would be ambiguous in the common case of an
         oscillating field within the unit cell.)

        In the case of inversion symmetry (!SCALAR_COMPLEX), we work with
        (real part - imag part) instead of (real part), to insure that we
        have something that is nonzero somewhere. */

     for (i = 0; i < N; ++i) {
#ifdef SCALAR_COMPLEX
	  real r = fabs(curfield[i].re * phase.re - curfield[i].im * phase.im);
#else
	  real r = fabs(curfield[i].re - curfield[i].im);
#endif
	  if (r > maxabs)
	       maxabs = r;
     }
     MPI_Allreduce(&maxabs, &maxabs, 1, SCALAR_MPI_TYPE,
		   MPI_MAX, MPI_COMM_WORLD);
     for (i = N - 1; i >= 0; --i) {
#ifdef SCALAR_COMPLEX
	  real r = curfield[i].re * phase.re - curfield[i].im * phase.im;
#else
	  real r = curfield[i].re - curfield[i].im;
#endif
	  if (fabs(r) >= 0.5 * maxabs) {
	       maxabs_index = i;
	       maxabs_sign = r < 0 ? -1 : 1;
	       break;
	  }
     }
     if (i >= 0)  /* convert index to global index in distributed array: */
	  maxabs_index += mdata->local_x_start * mdata->ny * mdata->nz;
     {
	  /* compute maximum index and corresponding sign over all the 
	     processors, using the MPI_MAXLOC reduction operation: */
	  struct {int i; int s;} x;
	  x.i = maxabs_index; x.s = maxabs_sign;
	  MPI_Allreduce(&x, &x, 1, MPI_2INT, MPI_MAXLOC, MPI_COMM_WORLD);
	  maxabs_index = x.i; maxabs_sign = x.s;
     }
     ASSIGN_SCALAR(phase,
		   SCALAR_RE(phase)*maxabs_sign, SCALAR_IM(phase)*maxabs_sign);

     printf("Fixing %c-field (band %d) phase by %g + %gi; max ampl. = %g\n",
	    curfield_type, curfield_band,
	    SCALAR_RE(phase), SCALAR_IM(phase), maxabs);

     /* Now, multiply everything by this phase, *including* the
	stored "raw" eigenvector in H, so that any future fields
	that we compute will have a consistent phase: */
     for (i = 0; i < N; ++i) {
	  real a,b;
	  a = curfield[i].re; b = curfield[i].im;
	  curfield[i].re = a*SCALAR_RE(phase) - b*SCALAR_IM(phase);
	  curfield[i].im = a*SCALAR_IM(phase) + b*SCALAR_RE(phase);
     }
     for (i = 0; i < H.n; ++i) {
          ASSIGN_MULT(H.data[i*H.p + curfield_band - 1], 
		      H.data[i*H.p + curfield_band - 1], phase);
     }
}

/**************************************************************************/

/* compute the fraction of the field energy that is located in the
   given range of dielectric constants: */
number compute_energy_in_dielectric(number eps_low, number eps_high)
{
     int N, i, last_dim, last_dim_stored;
     real *energy = (real *) curfield;
     real epsilon, energy_sum = 0.0;

     if (!curfield || !strchr("DH", curfield_type)) {
          fprintf(stderr, "The D or H energy density must be loaded first.\n");
          return 0.0;
     }

     N = mdata->fft_output_size;
     last_dim = mdata->last_dim;
     last_dim_stored =
	  mdata->last_dim_size / (sizeof(scalar_complex)/sizeof(scalar));
     for (i = 0; i < N; ++i) {
	  epsilon = 3.0 / (mdata->eps_inv[i].m00 +
			   mdata->eps_inv[i].m11 +
			   mdata->eps_inv[i].m22);
	  if (epsilon >= eps_low && epsilon <= eps_high) {
	       energy_sum += energy[i];
#ifndef SCALAR_COMPLEX
	       {
		    int last_index = i % last_dim_stored;
		    if (last_index != 0 && 2*last_index != last_dim)
			 energy_sum += energy[i];
	       }
#endif
	  }
     }
     MPI_Allreduce(&energy_sum, &energy_sum, 1, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     return energy_sum;
}

/**************************************************************************/

/* Prepend the prefix to the fname, and append a polarization
   specifier (if any) (e.g. ".te"), returning a new string, which
   should be deallocated with free().   fname or prefix may be NULL,
   in which case they are treated as the empty string. */
static char *fix_fname(const char *fname, const char *prefix,
		       polarization_t p)
{
     char *s;
     CHK_MALLOC(s, char,
		(fname ? strlen(fname) : 0) + 
		(prefix ? strlen(prefix) : 0) + 20);
     strcpy(s, prefix ? prefix : "");
     strcat(s, fname ? fname : "");
     if (p != NO_POLARIZATION) {
	  /* assumes polarization suffix is less than 20 characters;
	     currently it is less than 12 */
	  strcat(s, ".");
	  strcat(s, polarization_strings[p]);
     }
     return s;
}

/* given the field in curfield, store it to HDF (or whatever) using
   the matrixio (fieldio) routines.  Allow the user to specify that
   the fields be periodically extended, so that several lattice cells
   are stored.  Also allow the component to be specified
   (which_component 0/1/2 = x/y/z, -1 = all) for vector fields. */
void output_field_extended(vector3 copiesv, int which_component)
{
     char fname[100], *fname2, description[100];
     int dims[3], local_nx, local_x_start;
     int copies[3];
     matrixio_id file_id = -1;
     int attr_dims[2] = {3, 3};

     copies[0] = copiesv.x;
     copies[1] = copiesv.y;
     copies[2] = copiesv.z;

     if (!curfield) {
	  fprintf(stderr, 
		  "fields, energy dens., or epsilon must be loaded first.\n");
	  return;
     }
     
     /* this will need to be fixed for MPI, where we transpose the data */
#if defined(HAVE_MPI)
#  error broken, please fix
#endif
     dims[0] = mdata->nx;
     dims[1] = mdata->ny;
     dims[2] = mdata->nz;
     local_nx = mdata->local_nx;
     local_x_start = mdata->local_x_start;
     
     if (strchr("dhe", curfield_type)) { /* outputting vector field */
	  maxwell_vectorfield_makefull(mdata, curfield);

	  sprintf(fname, "%c.k%02d.b%02d",
		  curfield_type, kpoint_index, curfield_band);
	  if (which_component >= 0) {
	       char comp_str[] = ".x";
	       comp_str[1] = 'x' + which_component;
	       strcat(fname, comp_str);
	  }
	  sprintf(description, "%c field, kpoint %d, band %d, freq=%g",
		  curfield_type, kpoint_index, curfield_band, 
		  freqs.items[curfield_band - 1]);
	  fname2 = fix_fname(fname, filename_prefix, mdata->polarization);
	  printf("Outputting fields to %s...\n", fname2);
	  file_id = matrixio_create(fname2);
	  fieldio_write_complex_field(curfield, 3, dims, which_component,
				      local_nx, local_x_start,
				      copies, mdata->current_k, R, file_id);
	  free(fname2);
	  {
	       /* convert mdata->current_k back to the reciprocal basis */
	       real k[3] = {0,0,0};
	       int i, j;
	       for (i = 0; i < 3; ++i)
		    for (j = 0; j < 3; ++j)
			 k[i] += R[i][j] * mdata->current_k[j];
	       matrixio_write_data_attr(file_id, "Bloch wavevector",
					k, 1, attr_dims);
	  }
     }
     else if (strchr("DHn", curfield_type)) { /* scalar field */
	  maxwell_scalarfield_makefull(mdata, (real*) curfield);

	  if (curfield_type == 'n') {
	       sprintf(fname, "epsilon");
	       sprintf(description, "dielectric function, epsilon");
	  }
	  else {
	       sprintf(fname, "%cpwr.k%02d.b%02d",
		       tolower(curfield_type), kpoint_index, curfield_band);
	       sprintf(description,
		       "%c field energy density, kpoint %d, band %d, freq=%g",
		       curfield_type, kpoint_index, curfield_band, 
		       freqs.items[curfield_band - 1]);
	  }
	  fname2 = fix_fname(fname, filename_prefix, 
			     /* no polarization suffix for epsilon: */
			     curfield_type == 'n' ? NO_POLARIZATION :
			     mdata->polarization);
	  printf("Outputting %s...\n", fname2);
	  file_id = matrixio_create(fname2);
	  fieldio_write_real_vals((real *) curfield, 3, dims,
				  local_nx, local_x_start, copies, file_id);
	  free(fname2);
     }
     else
	  fprintf(stderr, "unknown field type!\n");

     if (file_id >= 0) {
	  real rcopies[3];

	  matrixio_write_data_attr(file_id, "lattice vectors",
				   &R[0][0], 2, attr_dims);
	  rcopies[0] = copies[0] < 1 ? 1 : copies[0];
	  rcopies[1] = copies[1] < 1 ? 1 : copies[1];
	  rcopies[2] = copies[2] < 1 ? 1 : copies[2];
	  matrixio_write_data_attr(file_id, "lattice copies",
				   rcopies, 1, attr_dims);
	  matrixio_write_string_attr(file_id, "description", description);

	  matrixio_close(file_id);
     }

#ifndef SCALAR_COMPLEX
     /* If we are using real-amplituded fields, then change curfield_type
	to reflect the fact that we have destroyed curfield.  (Or changed
	its format, anyway, with maxwell_*_makefull above.)  If people
	complain about this, we can always convert curfield back. */
     curfield_type = '-';
#endif
}

/**************************************************************************/

/* For curfield an energy density, compute the fraction of the energy
   that resides inside the given list of geometric objects.   Later
   objects in the list have precedence, just like the ordinary
   geometry list. */
number compute_energy_in_object_list(geometric_object_list objects)
{
     int i, j, k, n1, n2, n3, n_other, n_last, rank, last_dim;
     real s1, s2, s3, c1, c2, c3;
     real *energy = (real *) curfield;
     real energy_sum = 0;

     if (!curfield || !strchr("DH", curfield_type)) {
          fprintf(stderr, "The D or H energy density must be loaded first.\n");
          return 0.0;
     }

     n1 = mdata->nx; n2 = mdata->ny; n3 = mdata->nz;
     n_other = mdata->other_dims;
     n_last = mdata->last_dim_size / (sizeof(scalar_complex) / sizeof(scalar));
     last_dim = mdata->last_dim;
     rank = (n3 == 1) ? (n2 == 1 ? 1 : 2) : 3;

     s1 = geometry_lattice.size.x / n1;
     s2 = geometry_lattice.size.y / n2;
     s3 = geometry_lattice.size.z / n3;
     c1 = geometry_lattice.size.x * 0.5;
     c2 = geometry_lattice.size.y * 0.5;
     c3 = geometry_lattice.size.z * 0.5;

     /* Here we have different loops over the coordinates, depending
	upon whether we are using complex or real and serial or
        parallel transforms.  Each loop must define, in its body,
        variables (i2,j2,k2) describing the coordinate of the current
        point, and "index" describing the corresponding index in 
	the curfield array.

        This was all stolen from maxwell_eps.c...it would be better
        if we didn't have to cut and paste, sigh. */

#ifdef SCALAR_COMPLEX

#  ifndef HAVE_MPI
     
     for (i = 0; i < n1; ++i)
	  for (j = 0; j < n2; ++j)
	       for (k = 0; k < n3; ++k)
     {
	  int i2 = i, j2 = j, k2 = k;
	  int index = ((i * n2 + j) * n3 + k);

#  else /* HAVE_MPI */
#    error not yet implemented!
#  endif

#else /* not SCALAR_COMPLEX */

#  ifndef HAVE_MPI

     for (i = 0; i < n_other; ++i)
	  for (j = 0; j < n_last; ++j)
     {
	  int index = i * n_last + j;
	  int i2, j2, k2;
	  switch (rank) {
	      case 2: i2 = i; j2 = j; k2 = 0; break;
	      case 3: i2 = i / n2; j2 = i % n2; k2 = j; break;
	      default: i2 = j; j2 = k2 = 0;  break;
	  }

#  else /* HAVE_MPI */
#    error not yet implemented!
#  endif

#endif /* not SCALAR_COMPLEX */

	  {
	       vector3 p;
	       int n;
	       p.x = i2 * s1 - c1; p.y = j2 * s2 - c2; p.z = k2 * s3 - c3;
	       for (n = objects.num_items - 1; n >= 0; --n)
		    if (point_in_periodic_fixed_objectp(p, objects.items[n])) {
			 energy_sum += energy[index];
#ifndef SCALAR_COMPLEX
			 if (j != 0 && 2*j != last_dim)
			      energy_sum += energy[index];
#endif
			 break;
		    }
	  }
     }

     MPI_Allreduce(&energy_sum, &energy_sum, 1, SCALAR_MPI_TYPE,
                   MPI_SUM, MPI_COMM_WORLD);
     return energy_sum;
}

/**************************************************************************/
