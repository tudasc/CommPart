#ifndef CORRECTNESS_CHECKING_PARTITIONED_IMPL
#define CORRECTNESS_CHECKING_PARTITIONED_IMPL

#include "mpi.h"

typedef struct {
  MPI_Request request;
  void *buf_start;
  int partition_length;
  int partition_count;
  int partitions_ready;
  int is_active;
  int valgrind_block_handle;
} MPIX_Request;

int MPIX_Psend_init(void *buf, int partitions, MPI_Count count,
                    MPI_Datatype datatype, int dest, int tag, MPI_Comm comm,
                    MPI_Info info, MPIX_Request *request);
int MPIX_Precv_init(void *buf, int partitions, MPI_Count count,
                    MPI_Datatype datatype, int dest, int tag, MPI_Comm comm,
                    MPI_Info info, MPIX_Request *request);

int MPIX_Pready(int partition, MPIX_Request *request);
int MPIX_Pready_range(int partition_low, int partition_high,
                      MPIX_Request *request);

int MPIX_Start(MPIX_Request *request);
int MPIX_Wait(MPIX_Request *request, MPI_Status *status);

int MPIX_Request_free(MPIX_Request *request);

#endif
