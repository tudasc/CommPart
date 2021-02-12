#include "correctness-checking-partitioned-impl.h"
#include "assert.h"
#include "memcheck.h"
#include "mpi.h"

int MPIX_Psend_init(void *buf, int partitions, MPI_Count count,
                    MPI_Datatype datatype, int dest, int tag, MPI_Comm comm,
                    MPI_Info info, MPIX_Request *request) {

  // init request
  request->buf_start = buf;
  int size;
  MPI_Type_size(datatype, &size);
  request->partition_length = size * count;
  request->partition_count = partitions;
  request->partitions_ready = 0;
  request->is_active = 0;
  request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(
      buf, request->partition_length * request->partition_count,
      SEND_BLOCK_STRING);

  // init MPI
  return MPI_Send_init(buf, count * partitions, datatype, dest, tag, comm,
                       &request->request);
}

int MPIX_Precv_init(void *buf, int partitions, MPI_Count count,
                    MPI_Datatype datatype, int dest, int tag, MPI_Comm comm,
                    MPI_Info info, MPIX_Request *request) {
  // init request
  request->buf_start = buf;
  int size;
  MPI_Type_size(datatype, &size);
  request->partition_length = size * count;
  request->partition_count = partitions;
  request->partitions_ready = 0;
  request->is_active = 0;
  request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(
      buf, request->partition_length * request->partition_count,
      RECV_BLOCK_STRING);

  // init MPI
  return MPI_Recv_init(buf, count * partitions, datatype, dest, tag, comm,
                       &request->request);
}

int MPIX_Pready(int partition, MPIX_Request *request) {

  assert(request->is_active == 1);

  // taint partition as modification is forbidden
  VALGRIND_MAKE_MEM_NOACCESS(request->buf_start +
                                 request->partition_length * partition,
                             request->partition_length);
  // for a send operation reading is actually legal!!
  // valgrind does not support this fine grained analysis :-(
  // so we have to filter valgrinds errors based on the block names

#pragma omp atomic
  ++request->partitions_ready;

  return 0;
}

int MPIX_Pready_range(int partition_low, int partition_high,
                      MPIX_Request *request) {

  for (int i = partition_low; i <= partition_high; ++i) {
    MPIX_Pready(i, request);
  }

  return 0;
}

int MPIX_Start(MPIX_Request *request) {
  // do nothing now
  assert(request->is_active == 0);
  request->is_active = 1;
  assert(request->partitions_ready == 0);

  return 0;
}

int MPIX_Wait(MPIX_Request *request, MPI_Status *status) {

  assert(request->is_active == 1);
  assert(request->partition_count == request->partitions_ready);

  // now access is legal again
  VALGRIND_MAKE_MEM_DEFINED(request->buf_start, request->partition_length *
                                                    request->partition_count);

  // reset for next start call
  request->is_active = 0;
  request->partitions_ready = 0;

  MPI_Start(&request->request);
  // only start communication now, so that MPI itself does not interfere with
  // our memory access Analysis this way of implementing things is legal
  // according to the MPI standard anyway

  return MPI_Wait(&request->request, status);
}

int MPIX_Request_free(MPIX_Request *request) {
  assert(request->is_active == 0);

  VALGRIND_DISCARD(request->valgrind_block_handle);

  return MPI_Request_free(&request->request);
}
