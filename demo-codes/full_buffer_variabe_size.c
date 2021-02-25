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

	int buffer_size = atoi(argv[1]);

	assert(buffer_size>0);

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	//int pre = (rank == 0) ? -1 : rank - 1;
	//int nxt = (rank == size - 1) ? -1 : rank + 1;
	int nxt = (rank + 1) % size;
	int pre = (rank - 1) % size;
	pre = pre<0?size+pre:pre;// if % is negative: "start counting backwards at size"


	//printf("Rank %i in comm with %d and %d\n", rank, pre,nxt);

	int *buffer = (int*) malloc(sizeof(int) * buffer_size);
	int *buffer_r = (int*) malloc(sizeof(int) * buffer_size);

	// buffer access
#pragma omp parallel for firstprivate(buffer) schedule(static,1000)
//#pragma omp parallel for firstprivate(buffer) schedule(dynamic,1000)
//#pragma omp parallel for
	for (int i = 0; i < buffer_size; ++i) {
		buffer[i] = i * rank;
	}

	// communication

	// no deadlock
	MPI_Request req;

	printf("Rank %d recv from %d send to %d\n",rank,pre,nxt);
	MPI_Irecv(buffer_r,
			buffer_size, MPI_INT, pre, TAG,
		MPI_COMM_WORLD, &req);

	MPI_Send(buffer, buffer_size, MPI_INT, nxt, TAG,
	MPI_COMM_WORLD);

	MPI_Wait(&req, MPI_STATUS_IGNORE);


	free(buffer);
	free(buffer_r);

	MPI_Finalize();
}
