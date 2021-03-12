#ifndef CORRECTNESS_CHECKING_PARTITIONED_IMPL
#define CORRECTNESS_CHECKING_PARTITIONED_IMPL

// Includes the implementations in header, so that it may be included in
// user-code without any need for extra linking
#define INCLUDE_DEFINITION_IN_HEADER

// print debugging output and statistics
#define DEBUGING_PRINTINGS
// switch on the valgrind checking part
#define DO_VALGRIND_CHECKS

#include "mpi.h"

typedef struct {
  MPI_Request request;
  void *buf_start;
  int partition_length_bytes;
  int partition_count;
  int partitions_ready;
  int is_active;
#ifdef DO_VALGRIND_CHECKS
  int valgrind_block_handle;
#endif
  int dest;
  // to be used when there is a local overlap
  int* local_overlap;
  int* local_overlap_count;
  // information for the partitioning
  long A_min;
  long B_min;
  long A_max;
  long B_max;
  #ifdef DEBUGING_PRINTINGS
  int operation_number;
  #endif
  //MPI_Aint type_extend; // not needed anymore

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

#ifdef DO_VALGRIND_CHECKS
// definition of valgrind block names
#define SEND_BLOCK_STRING "SEND OPERATION: READING IS ALLOWED!"
#define RECV_BLOCK_STRING "RECEIVE OPERATION: READING IS FORBIDDEN"
#endif

#ifdef INCLUDE_DEFINITION_IN_HEADER
#include "correctness-checking-partitioned-impl.c"
#endif

#endif
