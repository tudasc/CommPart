/*
 Copyright 2021 Tim Jammer

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

#include "correctness-checking-partitioned-impl.h"
#include "assert.h"
#ifdef DO_VALGRIND_CHECKS
#include "memcheck.h"
#endif
#include "mpi.h"
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef DEBUGING_PRINTINGS
static int next_operation_number = 0;
#endif

int MPIX_Psend_init(void *buf, int partitions, MPI_Count count,
		MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info,
		MPIX_Request *request) {

	// some information set by_partition_ sending_op if needed, init it with 0
	memset(request, 0, sizeof(MPIX_Request));

	// init request
	request->buf_start = buf;
	MPI_Aint size;
	MPI_Type_extent(datatype, &size); //TODO with vector types this will give a lot f false positives (?)
	request->partition_length_bytes = size * count;
	request->partition_count = partitions;
	request->partitions_ready = 0;
	request->is_active = 0;
#ifdef DO_VALGRIND_CHECKS
	request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(buf,
			request->partition_length_bytes * request->partition_count,
			SEND_BLOCK_STRING);
#endif
	request->dest = dest;

#ifdef DEBUGING_PRINTINGS
// increment and assign
	request->operation_number = next_operation_number++;
#endif

	// init MPI
	return MPI_Send_init(buf, count * partitions, datatype, dest, tag, comm,
			&request->request);
}

int MPIX_Precv_init(void *buf, int partitions, MPI_Count count,
		MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info,
		MPIX_Request *request) {

	// some information set by_partition_ sending_op if needed, init it with 0
	memset(request, 0, sizeof(MPIX_Request));
	// init request
	request->buf_start = buf;
	MPI_Aint size;
	MPI_Type_extent(datatype, &size);
	request->partition_length_bytes = size * count;
	request->partition_count = partitions;
	request->partitions_ready = 0;
	request->is_active = 0;
#ifdef DO_VALGRIND_CHECKS
	request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(buf,
			request->partition_length_bytes * request->partition_count,
			RECV_BLOCK_STRING);
#endif

#ifdef DEBUGING_PRINTINGS
// increment and assign
	request->operation_number = next_operation_number++;
#endif

	// init MPI
	return MPI_Recv_init(buf, count * partitions, datatype, dest, tag, comm,
			&request->request);
}

int MPIX_Pready(int partition, MPIX_Request *request) {

	assert(request->is_active == 1);
#ifdef DO_VALGRIND_CHECKS
	// taint partition as modification is forbidden
	VALGRIND_MAKE_MEM_NOACCESS(
			((char* )request->buf_start)
					+ request->partition_length_bytes * partition,
			request->partition_length_bytes);
	// for a send operation reading is actually legal!!
	// valgrind does not support this fine grained analysis :-(
	// so we have to filter valgrinds errors based on the block names
#endif

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

	//TODO: with #pragma omp atomic capture
	//we can call the start part when the last thread signs off the partitions

	assert(request->is_active == 1);

	// if partition count ==1 then this should also function like a normal persistent operation
	if (request->partition_count == 1 && request->partitions_ready != 1) {
		MPIX_Pready(0, request);
	}
#ifdef DEBUGING_PRINTINGS
	printf("Operation %d: %d of %d Partitions are ready\n",
			request->operation_number, request->partitions_ready,
			request->partition_count);
#endif

	assert(request->partition_count == request->partitions_ready);

#ifdef DO_VALGRIND_CHECKS
	// now access is legal again
	VALGRIND_MAKE_MEM_DEFINED(request->buf_start,
			request->partition_length_bytes * request->partition_count);
#endif

	if (request->dest != MPI_PROC_NULL) {
		//TODO is this a bug in MPICH implementation?
		//as its segfaults if starting a request to MPI_PROC_NULL
		//TODO confirm that it is standard compilant to use proc null in persistent op and file bug report
		//TODO check with oter implementation such as openmpi

		MPI_Start(&request->request);
	}
	// only start communication now, so that MPI itself does not interfere with
	// our memory access Analysis this way of implementing things is legal
	// according to the MPI standard anyway

	// reset for next start call
	request->is_active = 0;
	request->partitions_ready = 0;

	// reset the local overlap information
	if (request->local_overlap) {
		memset(request->local_overlap, 0,
				sizeof(int) * request->partition_count);
	}
	if (request->dest != MPI_PROC_NULL) {
		return MPI_Wait(&request->request, status);
	} else
		return 0;
}

int MPIX_Request_free(MPIX_Request *request) {
	assert(request->is_active == 0);
#ifdef DO_VALGRIND_CHECKS
	VALGRIND_DISCARD(request->valgrind_block_handle);
#endif

	if (request->local_overlap) {
		free(request->local_overlap);
	}
	if (request->local_overlap_count) {
		free(request->local_overlap_count);
	}

	return MPI_Request_free(&request->request);
}

// current iter is the last index of current loop iteration+1 (upper bound)
int signoff_partitions_after_loop_iter(MPIX_Request *request,
// loop info
// access= pattern ax+b
		long min_iter, long max_iter) {

	// else: do nothing, mark message ready at wait call
	if (request->partition_count > 1) {

		//TODO is this sufficient? or do we need (ptr_arithmetic_t) ?
		long min_adress = request->A_min * min_iter + request->B_min;
		long max_adress = request->A_max * max_iter + request->B_max;

		//minimum_partition to sign off
		int min_part_num = (min_adress - (long) request->buf_start)
				/ request->partition_length_bytes;
		int max_part_num = (max_adress - (long) request->buf_start)
				/ request->partition_length_bytes;
		if ((max_adress - (long) request->buf_start)
				% request->partition_length_bytes == 0) {
			// Partition boundary itself belongs to the next partition
			--max_part_num;
		}

#ifdef DEBUGING_PRINTINGS
		printf(
				"Operation %d: Loop Part %ld to %ld : ready Partitions %d to %d \n",
				request->operation_number, min_iter, max_iter, min_part_num,
				max_part_num);
#endif

		// not outside of the boundaries of this operation
		min_part_num = min_part_num < 0 ? 0 : min_part_num;
		max_part_num =
				max_part_num > request->partition_count - 1 ?
						request->partition_count - 1 : max_part_num;

		// mark all involved partitions ready
		for (int i = min_part_num; i <= max_part_num; ++i) {

			int new_val;
// atomic add and fetch
#pragma omp atomic capture
			new_val = ++request->local_overlap[i];

			if (new_val == request->local_overlap_count[i]) {
				// other threads have also signed off
				MPIX_Pready(i, request);
			}
		}
	}

	return 1;
}

#ifdef DEBUGING_PRINTINGS
void debug_printing(MPI_Aint type_extned, long loop_max, long loop_min,
		long chunk_size, long A_min, long B_min, long A_max, long B_max,
		MPIX_Request *request) {
	//DEBUG PRINTING
	printf("Memory Layout for partitioned Operation:\n");
	char **msg_partitions = (char**) malloc(
			sizeof(char*) * (request->partition_count + 1));
	ptr_arithmetic_t *partition_adress = (ptr_arithmetic_t*) malloc(
			sizeof(ptr_arithmetic_t) * (request->partition_count + 1));

	for (int i = 0; i < request->partition_count; ++i) {
		partition_adress[i] = (ptr_arithmetic_t) request->buf_start
				+ (i * (ptr_arithmetic_t) request->partition_length_bytes);
		size_t needed_bytes = snprintf(NULL, 0, "Start MSG Part %i", i) + 1;
		msg_partitions[i] = (char*) malloc(needed_bytes);
		sprintf(msg_partitions[i], "Start MSG Part %i", i);
	}

	partition_adress[request->partition_count] = (long) request->buf_start
			+ request->partition_count * request->partition_length_bytes;
	size_t needed_bytes = snprintf(NULL, 0, "End of Message") + 1;
	msg_partitions[request->partition_count] = (char*) malloc(needed_bytes);
	sprintf(msg_partitions[request->partition_count], "End of Message");
	int chunks = (loop_max - loop_min + 1) / chunk_size;
	char **msg_chunks_begin = (char**) malloc(sizeof(char*) * chunks);
	ptr_arithmetic_t *chunk_adress_begin = (ptr_arithmetic_t*) malloc(
			sizeof(ptr_arithmetic_t) * chunks);
	char **msg_chunks_end = (char**) malloc(sizeof(char*) * chunks);
	ptr_arithmetic_t *chunk_adress_end = (ptr_arithmetic_t*) malloc(
			sizeof(ptr_arithmetic_t) * chunks);

	for (int i = 0; i < chunks; ++i) {
		unsigned long min_chunk_iter = loop_min + (i * chunk_size);
		unsigned long max_chunk_iter = loop_min + ((i + 1) * chunk_size);
		// not outside loop bounds
		assert(min_chunk_iter >= loop_min);	// otherwise makes no sense
		//min_chunk_iter = min_chunk_iter <loop_min ? loop_min : min_chunk_iter;
		max_chunk_iter = max_chunk_iter > loop_max ? loop_max : max_chunk_iter;
		chunk_adress_begin[i] = A_min * min_chunk_iter + B_min;
		chunk_adress_end[i] = A_max * max_chunk_iter + B_max;
		size_t needed_bytes = snprintf(NULL, 0, "Start Loop Chunk %i", i) + 1;
		msg_chunks_begin[i] = (char*) malloc(needed_bytes);
		sprintf(msg_chunks_begin[i], "Start Loop Chunk %i", i);
		needed_bytes = snprintf(NULL, 0, "End Loop Chunk %i", i) + 1;
		msg_chunks_end[i] = (char*) malloc(needed_bytes);
		sprintf(msg_chunks_end[i], "End Loop Chunk %i", i);
	}
	int current_chunk_begin = 0;
	int current_chunk_end = 0;
	int current_partition = 0;
	ptr_arithmetic_t base_adress = (ptr_arithmetic_t) request->buf_start;

	while (current_chunk_begin < chunks || current_chunk_end < chunks
			|| current_partition <= request->partition_count) {
		// ULONG_MAX *2 is enough to be larger than every ptr
		ptr_arithmetic_t curr_chunk_add_begin =
				current_chunk_begin < chunks ?
						chunk_adress_begin[current_chunk_begin] : ULONG_MAX * 2;
		ptr_arithmetic_t curr_chunk_add_end =
				current_chunk_end < chunks ?
						chunk_adress_end[current_chunk_end] : ULONG_MAX * 2;
		ptr_arithmetic_t curr_P =
				current_partition <= request->partition_count ?
						partition_adress[current_partition] : ULONG_MAX * 2;
		// lowest
		if (curr_chunk_add_begin < curr_chunk_add_end
				&& curr_chunk_add_begin < curr_P) {
			printf("0x%.8lX: %s (%ld)\n",
					(unsigned long) chunk_adress_begin[current_chunk_begin],
					msg_chunks_begin[current_chunk_begin],
					(long) (chunk_adress_begin[current_chunk_begin]
							- base_adress));
			free(msg_chunks_begin[current_chunk_begin]);
			current_chunk_begin++;
		} else if (curr_chunk_add_end < curr_P) {
			printf("0x%.8lX: %s (%ld)\n",
					(unsigned long) chunk_adress_end[current_chunk_end],
					msg_chunks_end[current_chunk_end],
					(long) (chunk_adress_end[current_chunk_end] - base_adress));
			free(msg_chunks_end[current_chunk_end]);
			current_chunk_end++;
		} else {
			printf("0x%.8lX: %s (%ld)\n",
					(unsigned long) partition_adress[current_partition],
					msg_partitions[current_partition],
					(long) (partition_adress[current_partition] - base_adress));
			free(msg_partitions[current_partition]);
			current_partition++;
		}
	}

	free(msg_chunks_begin);
	free(msg_chunks_end);
	free(msg_partitions);
	free(chunk_adress_begin);
	free(chunk_adress_end);
	free(partition_adress);

	printf("\n");
	printf("Partitions overlap_count:\n");
	for (int i = 0; i < request->partition_count; ++i) {
		printf("Partition %i : %i overlaps\n", i,
				request->local_overlap_count[i]);

	}
}
#endif

#define MAXIMUM_ITERATIONS 10

unsigned long find_valid_partition_size_bytes(long count, long type_extend,
		long requested_partition_size_bytes) {

	int rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);// only needed to govern debg printing so that only rank 0 prints
#ifdef DEBUGING_PRINTINGS
	if (rank == 0)
		printf(
				"find_valid Part size: count %ld, type %ld = %ldb,requested %ldb\n",
				count, type_extend, count * type_extend,
				requested_partition_size_bytes);
#endif

	long requested_partition_size_datamembers = requested_partition_size_bytes
			/ type_extend;
	// request more if necessary
	if (requested_partition_size_bytes % type_extend != 0) {
		++requested_partition_size_datamembers;
	}

	long sending_size_byte = count * type_extend;

	if (requested_partition_size_bytes > sending_size_byte) {
		return sending_size_byte;
	}

	int partition_size_canidate = requested_partition_size_datamembers;

	if (count % partition_size_canidate == 0) {
		return partition_size_canidate * type_extend;
	}

	int sign = 1;
	int offset = 1;

	long X_start = partition_size_canidate;

	int loop_count = 0;

	do {

		if (sign > 0) {
			partition_size_canidate = X_start + sign * offset;
			sign *= -1;
		}

		else {

			partition_size_canidate = X_start + sign * offset;
			sign *= -1;
			offset++;

		}
		loop_count++;

	} while (count % partition_size_canidate != 0
			|| loop_count < MAXIMUM_ITERATIONS);

	if (count % partition_size_canidate) {
		return partition_size_canidate * type_extend;
	} else {
#ifdef DEBUGING_PRINTINGS
		printf(
				"Was not able to calculate a good partition in %d Iterations: will not partiton this operation\n ",
				MAXIMUM_ITERATIONS);
#endif
		return sending_size_byte;
	}

}

//%partitions = call i32 @partition_sending_op(i8* %call3, i64 4000, i32 1275069445, i32 %rem, i32 42, i32 1140850688,
//%struct.MPIX_Request* %mpix_request,
//i64 4, i64 %5, i64 %7, i64 %9,
//i64 1000, i64 0, i64 3999)

// loop size is inclusive!
int partition_sending_op(void *buf, MPI_Count count, MPI_Datatype datatype,
		int dest, int tag, MPI_Comm comm, MPIX_Request *request,
		// loop info
		// access= pattern ax+b
		long curr_min, long next_min, long curr_max, long next_max,
		long chunk_size, long loop_min, long loop_max) {

	// calculate A and B based on the acces range of one chunk
	//TODO refactor to remove irrelevant calculation of A and B
	long A_min, B_min, A_max, B_max;
	B_min = curr_min;
	B_max = curr_max;

	A_min = (next_min - curr_min) / chunk_size;
	A_max = (next_max - curr_max) / chunk_size;

	int partitions = 1;

	int rank;
	MPI_Comm_rank(comm, &rank);	// only needed to govern debg printing so that only rank 0 prints

//#pragma omp single
	//{
	assert(A_min > 0 && "Decrementing loops not supported yet");
	assert(A_max > 0 && "Decrementing loops not supported yet");

	MPI_Aint type_extned, type_lb;
	//MPI_Type_extent(datatype, &type_extned);
	MPI_Type_get_extent(datatype, &type_lb, &type_extned);

	void *chunk_access_start;
	unsigned long chunk_access_length;
	long chunk_access_stride;	//  may be negative! == overlapping access

	long sending_size = type_extned * count;

	long access_size = (A_max * (loop_min + chunk_size) + B_max)
			- ((A_min * loop_min) + B_min);
#ifdef DEBUGING_PRINTINGS
	if (rank == 0) {
		printf("Try to partition this sending operation\n");
		printf(" loop size: %ld-%ld chunk:%ld\n", loop_min, loop_max,
				chunk_size);
		printf(" sending size: %ld access_size:%ld\n", sending_size,
				access_size);
		printf(" Ax+b: %ldx+%ld to %ldx+%ld\n", A_min, B_min, A_max, B_max);
		printf(" buf_start %lu count %lld\n", (unsigned long) buf, count);
	}
	assert(access_size == next_max - curr_min);
#endif

	if (access_size >= sending_size) {
		// no partitioning useful
#ifdef DEBUGING_PRINTINGS
		if (rank == 0)
			printf("Did not Partition Operation\n");
#endif
		assert(partitions == 1);
		MPIX_Psend_init(buf, partitions, count, datatype, dest, tag, comm,
		MPI_INFO_NULL, request);
		request->A_max = A_max;
		request->B_max = B_max;
		request->A_min = A_min;
		request->B_min = B_min;
		//request->type_extend = type_extned;

	} else {

		unsigned requested_partition_size_byte = access_size;

		unsigned long valid_partition_size_byte =
				find_valid_partition_size_bytes(count, type_extned,
						requested_partition_size_byte);
#ifdef DEBUGING_PRINTINGS
		if (rank == 0)
			printf("calculated Partition size: %lub\n",
					valid_partition_size_byte);
#endif

		unsigned valid_partition_size_datamembers = valid_partition_size_byte
				/ type_extned;

		assert(valid_partition_size_byte % type_extned == 0);

		partitions = count / valid_partition_size_datamembers;

		assert(count % valid_partition_size_datamembers == 0);
		assert(
				(partitions * valid_partition_size_datamembers * type_extned)
						== sending_size);

		assert((valid_partition_size_byte * partitions) == sending_size);
		assert(valid_partition_size_byte % type_extned == 0);
		assert((valid_partition_size_datamembers * partitions) == count);

		/*
		 int partition_size_datamembers = 0;
		 int partitions = 1;

		 if (partition_size_byte > type_extned) {
		 // larger: how many datamembers do we need per partition?
		 partitions = partition_size_byte / type_extned;
		 if (partition_size_byte % type_extned != 0) {
		 // we need full datamembers
		 partitions++;
		 }

		 } else if (partition_size_byte < type_extned) {
		 // smaller: each partition has 1 datamember
		 partitions = count;
		 partition_size_datamembers = 1;

		 } else {
		 // equals: each partition has 1 datamember
		 partitions = count;
		 partition_size_datamembers = 1;
		 }
		 */
#ifdef STATISTIC_PRINTINGS
		printf("Partitioned send operation into %d Partitions\n", partitions);
#endif
		MPIX_Psend_init(buf, partitions, valid_partition_size_datamembers,
				datatype, dest, tag, comm,
				MPI_INFO_NULL, request);
		request->A_max = A_max;
		request->B_max = B_max;
		request->A_min = A_min;
		request->B_min = B_min;
		//request->type_extend = type_extned;

		// calculate local overlap
		//TODO is there a better way than calculating it for each partition?
		// one can parallelize it at least?

		request->local_overlap = (int*) calloc(partitions, sizeof(int));
		request->local_overlap_count = (int*) malloc(partitions * sizeof(int));

		for (int i = 0; i < partitions; ++i) {
			ptr_arithmetic_t partition_min = (ptr_arithmetic_t) buf
					+ ((ptr_arithmetic_t) request->partition_length_bytes * i);
			ptr_arithmetic_t partition_max = partition_min
					+ (ptr_arithmetic_t) request->partition_length_bytes - 1;
			// boundary is exclusive

			// mem access = Ax+b ==> x = (mem-b)/A
			ptr_arithmetic_t min_loop_iter = (partition_min - B_min) / A_min;
			ptr_arithmetic_t max_loop_iter = (partition_max - B_max) / A_max;
			//TODO what if (mem-b)%A != 0 ?? is rounding down ok?

			// not outside loop bounds
			min_loop_iter = min_loop_iter < loop_min ? loop_min : min_loop_iter;
			max_loop_iter = max_loop_iter > loop_max ? loop_max : max_loop_iter;

			//if (rank==0) printf("Partition %i from loop iter %li to %li\n",i,min_loop_iter,max_loop_iter);

			ptr_arithmetic_t min_chunk = (min_loop_iter - loop_min)
					/ chunk_size;
			ptr_arithmetic_t max_chunk = (max_loop_iter - loop_min)
					/ chunk_size;
			// rounding down integer division is desired here
			//if (rank==0) printf("Partition %i from chunk %li to %li\n",i,min_chunk,max_chunk);
			// +1 as both numbers are inclusive
			request->local_overlap_count[i] = max_chunk - min_chunk + 1;

		}

#ifdef DEBUGING_PRINTINGS
		if (rank == 0) {
			debug_printing(type_extned, loop_max, loop_min, chunk_size, A_min,
					B_min, A_max, B_max, request);
		}
#endif

	}

//TODO which values can be inferred for the r/w mem access on the buffer regarding the loop index
	//}// end of pragma omp single

	return partitions;

}
