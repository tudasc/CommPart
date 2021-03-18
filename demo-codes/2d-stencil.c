#include "mpi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "correctness-checking-partitioned-impl.h"

// row-major order
// block_size + 2 for the halo lines
#define ind(i,j) (j)*(block_size+2)+(i)

#define TAG 42

int main(int argc, char **argv) {

	MPI_Init(&argc, &argv);
	int rank, numtasks;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
	int n, energy, niters, root_numtasks;
	double start_time = MPI_Wtime(); // time as early as possible so that overhead of partitioning is defenitely captured
	// default params
	n = 384 * 10000;
	energy = 256;
	niters = 1000;
	// argument checking
	if (argc >= 4) {
		n = atoi(argv[1]); // n times n grid
		energy = atoi(argv[2]); // energy to be injected per iteration
		niters = atoi(argv[3]); // number of iterations
	}

	root_numtasks = (int) sqrt(numtasks);

	// check if numtasks is a valid square and domain can be decomposed accordingly
	assert(root_numtasks * root_numtasks == numtasks);
	assert(n % root_numtasks == 0);

	// processes form a square of length root_numtasks,
	// determine my four neighbors
	int north = rank - root_numtasks;
	int south = rank + root_numtasks;
	int west = rank - 1;
	int east = rank + 1;
	// invalid neighbor means no neighbor
	if (north < 0 || north >= numtasks)
		north = MPI_PROC_NULL;
	if (south < 0 || south >= numtasks)
		south = MPI_PROC_NULL;
	if (west < 0 || west >= numtasks)
		west = MPI_PROC_NULL;
	if (east < 0 || east >= numtasks)
		east = MPI_PROC_NULL;

	const int block_size = n / root_numtasks;

	if (rank==0) printf("Block_size = %d(*8)",block_size);

	// each process owns one heat source with random placement
	// +1 for the halo line
	const int local_heat_source_x = 1 + (rand() % block_size);
	const int local_heat_source_y = 1 + (rand() % block_size);

	// allocate two work arrays for local part
	// block_length+2 in both directions for the halo lines
	double *aold = (double*) calloc(1,
			(block_size + 2) * (block_size + 2) * sizeof(double)); // 1-wide halo zones!
	double *anew = (double*) calloc(1,
			(block_size + 2) * (block_size + 2) * sizeof(double)); // 1-wide halo zones!

	/*
					 if (rank == 0) {
					 printf("Grid of %i times %i processes\nwith block of %i times %i\n",
					 root_numtasks, root_numtasks, block_size, block_size);
					 }
					 */

	// datatypes for halo exchange
	MPI_Datatype full_line_type;
	MPI_Type_contiguous(block_size, MPI_DOUBLE, &full_line_type);
	MPI_Type_commit(&full_line_type);

	MPI_Datatype first_element_of_line_type;
	// create a padded type to only sent only the first element of a line
	MPI_Type_create_resized(MPI_DOUBLE,0,(block_size + 2)*sizeof(double),&first_element_of_line_type);
	MPI_Type_commit(&first_element_of_line_type);
	//https://pages.tacc.utexas.edu/~eijkhout/pcse/html/mpi-data.html#Typeextent


	// again +2 for the halo area
	// full line does not need this halo part, as it is only used to send one line at a time
	// if one wants to use it for sending multiple lines at a time better use:
	//MPI_Type_vector(1, block_length, block_length+2, MPI_DOUBLE, &full_line_type);

	MPI_Request reqs[8] = { MPI_REQUEST_NULL, MPI_REQUEST_NULL,
			MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL,
			MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL };

	double init_time = MPI_Wtime();
	double heat; // total heat in system

	for (int iter = 0; iter < niters; ++iter) {

		// introduce new heat
		aold[ind(local_heat_source_x, local_heat_source_y)] += energy;

		heat = 0.0;
// the memory pointed to by anew and aold is shared but the ptr itself can be firstprivate
#pragma omp parallel for firstprivate(anew,aold,block_size) schedule(static,1000) reduction(+:heat) default(none)
		for (int j = 1; j < block_size + 1; ++j) {
			for (int i = 1; i < block_size + 1; ++i) {
				// stencil
				anew[ind(i, j)] = aold[ind(i, j)] / 2.0
						+ (aold[ind(i - 1, j)] + aold[ind(i + 1, j)]
								+ aold[ind(i, j - 1)] + aold[ind(i, j + 1)])
								/ 4.0 / 2.0;
				heat += anew[ind(i, j)];
			}
		}

		// exchange data with neighbors
		//MPI_Request reqs[4];

		MPI_Irecv(&anew[ind(1, 0)], 1, full_line_type, north, TAG,
		MPI_COMM_WORLD, &reqs[0]);
		MPI_Irecv(&anew[ind(1, block_size + 1)], 1, full_line_type, south,
		TAG, MPI_COMM_WORLD, &reqs[1]);
		MPI_Irecv(&anew[ind(block_size + 1, 1)], block_size,
				first_element_of_line_type, east, TAG,
				MPI_COMM_WORLD, &reqs[2]);
		MPI_Irecv(&anew[ind(0, 1)], block_size, first_element_of_line_type,
				west, TAG, MPI_COMM_WORLD, &reqs[3]);
		//TODO try moving before loop if my analysis doesnt break?


		//TODO nested loop fehler!
		// 64k is actually right!
		// correctly not partitioning this send ops
		MPI_Isend(&anew[ind(1, 1)], 1, full_line_type, north, TAG,
				MPI_COMM_WORLD, &reqs[4]);
		MPI_Isend(&anew[ind(1, block_size)], 1, full_line_type, south, TAG,
				MPI_COMM_WORLD, &reqs[5]);

		//TODO this send op is flawed
		MPI_Isend(&anew[ind(block_size, 1)], block_size,
				first_element_of_line_type, east, TAG, MPI_COMM_WORLD,
				&reqs[6]);
		//This send op partitions just fine!
		MPI_Isend(&anew[ind(1, 1)], block_size, first_element_of_line_type,
				west, TAG, MPI_COMM_WORLD, &reqs[7]);

		MPI_Waitall(8, reqs, MPI_STATUSES_IGNORE);

		// swap arrays
		// swap with memcpy as swapping of pointer currently breaks the partitioning analysis
		memcpy(aold, anew,
				(block_size + 2) * (block_size + 2) * sizeof(double));

	}
	double end_time = MPI_Wtime();

	MPI_Type_free(&full_line_type);
	MPI_Type_free(&first_element_of_line_type);

	// get final heat in the system
	double total_heat;
	MPI_Allreduce(&heat, &total_heat, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	if (rank == 0)
		printf("init time: %fs\n",
				 init_time - start_time);
		printf("after %i iterations:\ntotal heat: %f computation time: %fs\n", niters,
				total_heat, end_time - init_time);

	MPI_Finalize();
}
