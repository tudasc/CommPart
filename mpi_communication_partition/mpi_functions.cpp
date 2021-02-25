/*
 Copyright 2020 Tim Jammer

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

#include "mpi_functions.h"
#include <assert.h>

#include "llvm/IR/InstrTypes.h"
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

bool is_mpi_call(CallBase *call) {
	if (call->getCalledFunction()) {
		return is_mpi_function(call->getCalledFunction());
	} else {
		return false;
	} // for calls like call i64 asm sideeffect
}

bool is_mpi_function(llvm::Function *f) {
	return f->getName().contains("MPI");

}

struct mpi_functions* get_used_mpi_functions(llvm::Module &M) {

	struct mpi_functions *result = new struct mpi_functions;
	assert(result != nullptr);

	for (auto it = M.begin(); it != M.end(); ++it) {
		Function *f = &*it;
		if (f->getName().equals("MPI_Init")) {
			result->mpi_init = f;
			// should not be called twice anyway, so no need to handle it
			// result->conflicting_functions.insert(f);

			// sync functions:
		} else if (f->getName().equals("MPI_Finalize")) {
			result->mpi_finalize = f;
			result->sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Barrier")) {
			result->mpi_barrier = f;
			result->sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Ibarrier")) {
			result->mpi_Ibarrier = f;
			result->sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Allreduce")) {
			result->mpi_allreduce = f;
			result->sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Iallreduce")) {
			result->mpi_Iallreduce = f;
			result->sync_functions.insert(f);
		}

		// different sending modes:
		else if (f->getName().equals("MPI_Send")) {
			result->mpi_send = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Bsend")) {
			result->mpi_Bsend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Ssend")) {
			result->mpi_Ssend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Rsend")) {
			result->mpi_Rsend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Isend")) {
			result->mpi_Isend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Ibsend")) {
			result->mpi_Ibsend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Issend")) {
			result->mpi_Issend = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Irsend")) {
			result->mpi_Irsend = f;
			result->conflicting_functions.insert(f);

		} else if (f->getName().equals("MPI_Sendrecv")) {
			result->mpi_Sendrecv = f;
			result->conflicting_functions.insert(f);

		} else if (f->getName().equals("MPI_Recv")) {
			result->mpi_recv = f;
			result->conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Irecv")) {
			result->mpi_Irecv = f;
			result->conflicting_functions.insert(f);

			// Other MPI functions, that themselves may not yield another conflict
		} else if (f->getName().equals("MPI_Buffer_detach")) {
			result->mpi_buffer_detach = f;
			result->unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Test")) {
			result->mpi_test = f;
			result->unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Wait")) {
			result->mpi_wait = f;
			result->unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Waitall")) {
			result->mpi_waitall = f;
			result->unimportant_functions.insert(f);

			//self implemented Partitioned functions
			// search for either the normal name (C mode) or the mangled name (C++ mode)
		} else if (f->getName().equals("MPIX_Psend_init")|| f->getName().equals("_Z15MPIX_Psend_initPvixiiiiiP12MPIX_Request")) {
			result->mpix_Psend_init = f;
		} else if (f->getName().equals("MPIX_Precv_init")|| f->getName().equals("_Z15MPIX_Precv_initPvixiiiiiP12MPIX_Request")) {
			result->mpix_Precv_init = f;
		} else if (f->getName().equals("MPIX_Start")|| f->getName().equals("_Z10MPIX_StartP12MPIX_Request")) {
			result->mpix_Start = f;
		} else if (f->getName().equals("MPIX_Wait")|| f->getName().equals("_Z9MPIX_WaitP12MPIX_RequestP10MPI_Status")) {
			result->mpix_Wait = f;

		} else if (f->getName().equals("MPIX_Pready")|| f->getName().equals("_Z11MPIX_PreadyiP12MPIX_Request")) {
			result->mpix_Pready = f;
		} else if (f->getName().equals("MPIX_Pready_range")|| f->getName().equals("_Z17MPIX_Pready_rangeiiP12MPIX_Request")) {
			result->mpix_Pready_range = f;
		} else if (f->getName().equals("MPIX_Request_free")
				|| f->getName().equals("_Z17MPIX_Request_freeP12MPIX_Request")) {
			result->mpix_Request_free = f;

			// library funcs to handle the partitioning
		} else if (f->getName().equals("partition_sending_op")
				|| f->getName().equals(
						"_Z20partition_sending_opPvxiiiiP12MPIX_Requestlllllll")) {
			result->partition_sending_op = f;

		} else if (f->getName().equals("signoff_partitions_after_loop_iter")
				|| f->getName().equals(
						"_Z34signoff_partitions_after_loop_iterP12MPIX_Requestll")) {
			result->signoff_partitions_after_loop_iter = f;
		}
	}

	//check if all defined functions are present
	bool all_present = (result->mpix_Psend_init != nullptr)
			&& (result->mpix_Precv_init != nullptr)
			&& (result->mpix_Start != nullptr) && (result->mpix_Wait != nullptr)
			&& (result->mpix_Pready != nullptr)
			&& (result->mpix_Pready_range != nullptr)
			&& (result->mpix_Request_free != nullptr);
	assert(
			result->partition_sending_op != nullptr
					&& result->signoff_partitions_after_loop_iter != nullptr);

	assert(
			all_present
					&& "Could not find all operations for Partitioned communication to be defined");

	// get type of mpix request for allocation of needed requests
	PointerType *pointer_t = dyn_cast<PointerType>(
			result->mpix_Request_free->getFunctionType()->getParamType(0));
	result->mpix_request_type = pointer_t->getElementType();
	assert(result->mpix_request_type);

	return result;
}

bool is_mpi_used(struct mpi_functions *mpi_func) {

	if (mpi_func->mpi_init != nullptr) {
		return mpi_func->mpi_init->getNumUses() > 0;
	} else {
		return false;
	}
}

bool is_send_function(llvm::Function *f) {
	assert(f != nullptr);
	return f == mpi_func->mpi_send || f == mpi_func->mpi_Bsend
			|| f == mpi_func->mpi_Ssend || f == mpi_func->mpi_Rsend
			|| f == mpi_func->mpi_Isend || f == mpi_func->mpi_Ibsend
			|| f == mpi_func->mpi_Irsend || f == mpi_func->mpi_Issend
			|| f == mpi_func->mpi_Sendrecv;
}

bool is_recv_function(llvm::Function *f) {
	assert(f != nullptr);
	return f == mpi_func->mpi_recv || f == mpi_func->mpi_Irecv
			|| f == mpi_func->mpi_Sendrecv;
}
