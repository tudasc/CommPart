#include "correctness-checking-partitioned-impl.h"
#include "assert.h"
#include "memcheck.h"
#include "mpi.h"
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int MPIX_Psend_init(void *buf, int partitions, MPI_Count count,
		MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info,
		MPIX_Request *request) {

	// init request
	request->buf_start = buf;
	int size;
	MPI_Type_size(datatype, &size);
	request->partition_length = size * count;
	request->partition_count = partitions;
	request->partitions_ready = 0;
	request->is_active = 0;
	request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(buf,
			request->partition_length * request->partition_count,
			SEND_BLOCK_STRING);

	// init MPI
	return MPI_Send_init(buf, count * partitions, datatype, dest, tag, comm,
			&request->request);
}

int MPIX_Precv_init(void *buf, int partitions, MPI_Count count,
		MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info,
		MPIX_Request *request) {
	// init request
	request->buf_start = buf;
	int size;
	MPI_Type_size(datatype, &size);
	request->partition_length = size * count;
	request->partition_count = partitions;
	request->partitions_ready = 0;
	request->is_active = 0;
	request->valgrind_block_handle = VALGRIND_CREATE_BLOCK(buf,
			request->partition_length * request->partition_count,
			RECV_BLOCK_STRING);

	// init MPI
	return MPI_Recv_init(buf, count * partitions, datatype, dest, tag, comm,
			&request->request);
}

int MPIX_Pready(int partition, MPIX_Request *request) {

	assert(request->is_active == 1);

	// taint partition as modification is forbidden
	VALGRIND_MAKE_MEM_NOACCESS(
			((char* )request->buf_start)
					+ request->partition_length * partition,
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

	//TODO: with #pragma omp atomic capture
	//we can call the start part when the last thread signs off the partitions

	assert(request->is_active == 1);
	assert(request->partition_count == request->partitions_ready);

	// now access is legal again
	VALGRIND_MAKE_MEM_DEFINED(request->buf_start,
			request->partition_length * request->partition_count);

	MPI_Start(&request->request);
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

	return MPI_Wait(&request->request, status);
}

int MPIX_Request_free(MPIX_Request *request) {
	assert(request->is_active == 0);

	VALGRIND_DISCARD(request->valgrind_block_handle);

	if (request->local_overlap) {
		free(request->local_overlap);
	}
	if (request->local_overlap_count) {
		free(request->local_overlap_count);
	}

	return MPI_Request_free(&request->request);
}

// current iter is the last index of current loop iteration+1 (upper bound)
int signoff_partitions_after_loop_iter(long current_iter, MPIX_Request *request,
		MPI_Datatype datatype,
		// loop info
		// access= pattern ax+b
		long A, long B, long local_min, long local_max) {

	long min_adress = A * local_min + B;
	long max_adress = A * local_max + B;

	MPI_Aint type_extned;
	MPI_Type_extent(datatype, &type_extned);

	//minimum_partition to sign off
	int min_part_num = (min_adress - (long) request->buf_start)
			/ request->partition_length;
	int max_part_num = (max_adress - (long) request->buf_start)
			/ request->partition_length;
	if ((max_adress - (long) request->buf_start) % request->partition_length
			!= 0) {
		max_part_num++;
	}

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

	return 1;
}

void debug_printing(MPI_Aint type_extned, long loop_max, long loop_min,
		long chunk_size, long A_min, long B_min, long A_max, long B_max,
		MPIX_Request *request) {
	//DEBUG PRINTING
	printf("Memory Layout for partitioned Operation:\n");
	char **msg_partitions = malloc(
			sizeof(char*) * (request->partition_count + 1));
	long *partition_adress = malloc(
			sizeof(long) * (request->partition_count + 1));
	for (int i = 0; i < request->partition_count; ++i) {
		partition_adress[i] = (long) request->buf_start
				+ i * type_extned * request->partition_length;
		size_t needed_bytes = snprintf(NULL, 0, "Start MSG Part %i\n", i) + 1;
		msg_partitions[i] = malloc(needed_bytes);
		sprintf(msg_partitions[i], "Start MSG Part %i\n", i);
	}
	partition_adress[request->partition_count] = (long) request->buf_start
			+ request->partition_count * type_extned
					* request->partition_length;
	size_t needed_bytes = snprintf(NULL, 0, "End of Message") + 1;
	msg_partitions[request->partition_count] = malloc(needed_bytes);
	sprintf(msg_partitions[request->partition_count], "End of Message");
	int chunks = (loop_max - loop_min) / chunk_size;
	char **msg_chunks_begin = malloc(sizeof(char*) * chunks);
	long *chunk_adress_begin = malloc(sizeof(long) * chunks);
	char **msg_chunks_end = malloc(sizeof(char*) * chunks);
	long *chunk_adress_end = malloc(sizeof(long) * chunks);
	for (int i = 0; i < chunks; ++i) {
		long min_chunk_iter = loop_min + i * chunk_size;
		long max_chunk_iter = loop_min + i * (chunk_size + 1);
		// not outside loop bounds
		min_chunk_iter = min_chunk_iter < loop_min ? loop_min : min_chunk_iter;
		max_chunk_iter = max_chunk_iter < loop_max ? loop_max : max_chunk_iter;
		chunk_adress_begin[i] = A_min * min_chunk_iter + B_min;
		chunk_adress_end[i] = A_max * max_chunk_iter + B_max;
		size_t needed_bytes = snprintf(NULL, 0, "Start Loop Chunk %i\n", i) + 1;
		msg_chunks_begin[i] = malloc(needed_bytes);
		sprintf(msg_chunks_begin[i], "Start Loop Chunk %i\n", i);
		needed_bytes = snprintf(NULL, 0, "End Loop Chunk %i\n", i) + 1;
		msg_chunks_end[i] = malloc(needed_bytes);
		sprintf(msg_chunks_end[i], "End Loop Chunk %i\n", i);
	}
	int current_chunk_begin = 0;
	int current_chunk_end = 0;
	int current_partition = 0;
	while (current_chunk_begin < chunks || current_chunk_end < chunks
			|| current_partition <= request->partition_count) {
		long curr_chunk_add_begin =
				current_chunk_begin < chunks ?
						chunk_adress_begin[current_chunk_begin] : LONG_MAX;
		long curr_chunk_add_end =
				current_chunk_end < chunks ?
						chunk_adress_end[current_chunk_end] : LONG_MAX;
		long curr_P =
				current_partition <= request->partition_count ?
						partition_adress[current_partition] : LONG_MAX;
		// lowest
		if (curr_chunk_add_begin < curr_chunk_add_end
				&& curr_chunk_add_begin < curr_P) {
			printf("0x%.8lX: %s", chunk_adress_begin[current_chunk_begin],
					msg_chunks_begin[current_chunk_begin]);
			current_chunk_begin++;
		} else if (curr_chunk_add_end < curr_P) {
			printf("0x%.8lX: %s", chunk_adress_end[current_chunk_end],
					msg_chunks_end[current_chunk_end]);
			current_chunk_end++;
		} else {
			printf("0x%.8lX: %s", partition_adress[current_partition],
					msg_partitions[current_partition]);
			current_partition++;
		}
	}
}

int partition_sending_op(void *buf, MPI_Count count, MPI_Datatype datatype,
		int dest, int tag, MPI_Comm comm, MPIX_Request *request,
		// loop info
		// access= pattern ax+b
		long A_min, long B_min, long A_max, long B_max, long chunk_size,
		long loop_min, long loop_max) {

	assert(A_min > 0 && "Decrementing loops not supported yet");
	assert(A_max > 0 && "Decrementing loops not supported yet");

	void *chunk_access_start;
	unsigned long chunk_access_length;
	long chunk_access_stride;		//  may be negative! == overlapping access

	MPI_Aint type_extned;
	MPI_Type_extent(datatype, &type_extned);

	long sending_size = type_extned * count;

	long access_size = A_max * (loop_min + chunk_size) + B_max
			+ -A_min * loop_min + B_min;

	if (access_size >= sending_size) {
		// no partitioning useful
		int partitions = 1;

		MPIX_Psend_init(buf, partitions, count, datatype, dest, tag, comm,
		MPI_INFO_NULL, request);

		return partitions;

	} else {

		// int division: rounding down is implicit
		unsigned requested_partition_size_byte = sending_size / access_size;

		unsigned requested_partition_size_datamembers =
				(requested_partition_size_byte / type_extned);
		if (requested_partition_size_byte % type_extned != 0) {
			requested_partition_size_datamembers++;
		}
		requested_partition_size_byte = requested_partition_size_datamembers
				* type_extned;

		unsigned valid_partition_size_byte = type_extned;
		unsigned valid_partition_size_datamembers = 1;
		int partitions = count;

		//TODO calculate this!

		assert(valid_partition_size_byte % sending_size == 0);

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

		printf("Partitioned send operations into %d Partitions\n", partitions);
		MPIX_Psend_init(buf, partitions, valid_partition_size_datamembers,
				datatype, dest, tag, comm,
				MPI_INFO_NULL, request);

		// calculate local overlap
		//TODO is there a better way than calculating it for each partition?
		// one can parallelize it at least?

		request->local_overlap = calloc(partitions, sizeof(int));
		request->local_overlap_count = malloc(partitions * sizeof(int));

		for (int i = 0; i < partitions; ++i) {
			long partition_min = (long) buf
					+ (request->partition_length * type_extned) * i;
			long partition_max = partition_min
					+ request->partition_length * type_extned;

			long min_loop_iter = (partition_min - B_min) / A_min;
			long max_loop_iter = (partition_max - B_max) / A_max;

			// not outside loop bounds
			min_loop_iter = min_loop_iter < loop_min ? loop_min : min_loop_iter;
			max_loop_iter = max_loop_iter < loop_max ? loop_max : max_loop_iter;

			long min_chunk = (min_loop_iter - loop_min) / chunk_size;
			long max_chunk = (max_loop_iter - loop_min) / chunk_size;
			//if partly in next chunk
			if ((max_loop_iter - loop_min) % chunk_size != 0) {
				max_chunk++;
			}
			// +1 as both numbers are inclusive
			request->local_overlap_count[i] = max_chunk - min_chunk + 1;

		}

		//DEBUG PRINTING
		debug_printing(type_extned, loop_max, loop_min, chunk_size, A_min,
				B_min, A_max, B_max, request);
	}

//TODO which values can be inferred for the r/w mem access on the buffer regarding the loop index

	int partitions = 1;

	MPIX_Psend_init(buf, partitions, count, datatype, dest, tag, comm,
	MPI_INFO_NULL, request);

	return partitions;

}
