#include "correctness-checking-partitioned-impl.h"
#include "mpi.h"

#include <stdio.h>
#include <stdlib.h>

#define TOTAL_SIZE 4000
#define ITERATIONS 10
#define TAG 42

//buffer:
// RECV SEND LOCAL SEND RECV
// TOTAL_SIZE must be at least 4 times STENCIL_SIZE

void debug_function(long a, long b) {
	printf(" %ld,%ld\n", a, b);
}

int main(int argc, char **argv) {

	MPI_Init(&argc, &argv);

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	//int pre = (rank == 0) ? -1 : rank - 1;
	//int nxt = (rank == size - 1) ? -1 : rank + 1;
	int nxt = size % (rank + 1);

	//printf("Rank %i in comm with %d and %d\n", rank, pre,nxt);

	int *buffer = (int*) malloc(sizeof(int) * TOTAL_SIZE);
	int *buffer_r = (int*) malloc(sizeof(int) * TOTAL_SIZE);

	// buffer access
#pragma omp parallel for firstprivate(buffer) schedule(static,1000)
//#pragma omp parallel for
	for (int i = 0; i < TOTAL_SIZE; ++i) {
		buffer[i] = i * rank;
	}

	// communication

	MPI_Send(buffer, TOTAL_SIZE, MPI_INT, nxt, TAG,
	MPI_COMM_WORLD);
	MPI_Recv(buffer_r,
	TOTAL_SIZE, MPI_INT, nxt, TAG,
	MPI_COMM_WORLD, MPI_STATUS_IGNORE);

	free(buffer);
	free(buffer_r);

	MPI_Finalize();
}
