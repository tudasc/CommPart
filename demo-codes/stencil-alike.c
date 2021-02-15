#include "correctness-checking-partitioned-impl.h"
#include "mpi.h"

#include <stdio.h>
#include <stdlib.h>

#define STENCIL_SIZE 1024
#define TOTAL_SIZE (STENCIL_SIZE * 5)
#define ITERATIONS 10
#define TAG 42

//buffer:
// RECV SEND LOCAL SEND RECV
// TOTAL_SIZE must be at least 4 times STENCIL_SIZE

int main(int argc, char **argv) {

	assert(TOTAL_SIZE >= STENCIL_SIZE*4);
	MPI_Init(&argc, &argv);

	const int LOCAL_SIZE = TOTAL_SIZE - 4 * STENCIL_SIZE;

	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	int pre = (rank == 0) ? -1 : rank - 1;
	int nxt = (rank == size - 1) ? -1 : rank + 1;

	//printf("Rank %i in comm with %d and %d\n", rank, pre,nxt);

	int *buffer = (int*) malloc(sizeof(int) * TOTAL_SIZE);

	// init
#pragma omp parallel for
	for (int i = 0; i < TOTAL_SIZE; ++i) {
		buffer[i] = i * rank;
	}

	for (int iter = 0; iter < ITERATIONS; ++iter) {

		int max = 0;
		// some computation
#pragma omp parallel for reduction (max:max)
		for (int i = 1; i < TOTAL_SIZE - 1; ++i) {
			buffer[i] = buffer[i - 1] + buffer[i] - buffer[i + 1];
			max = buffer[i] > max ? buffer[i] : max;
		}


		// communication
		if (rank % 2 == 0) {
			if (pre != -1) {
				MPI_Send(buffer, STENCIL_SIZE, MPI_INT, pre, TAG,
				MPI_COMM_WORLD);
				MPI_Recv(buffer + STENCIL_SIZE, STENCIL_SIZE, MPI_INT, pre, TAG,
				MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
			if (nxt != -1) {
				MPI_Send(
						buffer + STENCIL_SIZE + STENCIL_SIZE + LOCAL_SIZE
								+ STENCIL_SIZE, STENCIL_SIZE, MPI_INT, nxt, TAG,
						MPI_COMM_WORLD);
				MPI_Recv(buffer + STENCIL_SIZE + STENCIL_SIZE + LOCAL_SIZE,
				STENCIL_SIZE, MPI_INT, nxt, TAG,
				MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
		} else {
			// recv first to avoid deadlock
			if (pre != -1) {
				MPI_Recv(buffer + STENCIL_SIZE, STENCIL_SIZE, MPI_INT, pre, TAG,
				MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				MPI_Send(buffer, STENCIL_SIZE, MPI_INT, pre, TAG,
				MPI_COMM_WORLD);

			}
			if (nxt != -1) {
				MPI_Recv(buffer + STENCIL_SIZE + STENCIL_SIZE + LOCAL_SIZE,
				STENCIL_SIZE, MPI_INT, nxt, TAG,
				MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				MPI_Send(
						buffer + STENCIL_SIZE + STENCIL_SIZE + LOCAL_SIZE
								+ STENCIL_SIZE, STENCIL_SIZE, MPI_INT, nxt, TAG,
						MPI_COMM_WORLD);
			}
		}

		if (rank == 0) {
			MPI_Reduce(MPI_IN_PLACE,&max, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
			printf("Iteration %i (max %i)\n", iter, max);
		}else{
		MPI_Reduce(&max, NULL, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
		}

	}

	free(buffer);

	MPI_Finalize();
}
