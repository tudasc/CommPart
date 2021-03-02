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

bool MpiFunctions::is_mpi_call(CallBase *call) {
	if (call->getCalledFunction()) {
		return is_mpi_function(call->getCalledFunction());
	} else {
		return false;
	} // for calls like call i64 asm sideeffect
}

bool MpiFunctions::is_mpi_function(llvm::Function *f) {
	return f->getName().contains("MPI");

}

MpiFunctions::MpiFunctions(llvm::Module &M) {

	for (auto it = M.begin(); it != M.end(); ++it) {
		Function *f = &*it;
		if (f->getName().equals("MPI_Init")) {
			mpi_init = f;
			// should not be called twice anyway, so no need to handle it
			// conflicting_functions.insert(f);

			// sync functions:
		} else if (f->getName().equals("MPI_Finalize")) {
			mpi_finalize = f;
			sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Barrier")) {
			mpi_barrier = f;
			sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Ibarrier")) {
			mpi_Ibarrier = f;
			sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Allreduce")) {
			mpi_allreduce = f;
			sync_functions.insert(f);
		} else if (f->getName().equals("MPI_Iallreduce")) {
			mpi_Iallreduce = f;
			sync_functions.insert(f);
		}

		// different sending modes:
		else if (f->getName().equals("MPI_Send")) {
			mpi_send = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Bsend")) {
			mpi_Bsend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Ssend")) {
			mpi_Ssend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Rsend")) {
			mpi_Rsend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Isend")) {
			mpi_Isend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Ibsend")) {
			mpi_Ibsend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Issend")) {
			mpi_Issend = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Irsend")) {
			mpi_Irsend = f;
			conflicting_functions.insert(f);

		} else if (f->getName().equals("MPI_Sendrecv")) {
			mpi_Sendrecv = f;
			conflicting_functions.insert(f);

		} else if (f->getName().equals("MPI_Recv")) {
			mpi_recv = f;
			conflicting_functions.insert(f);
		} else if (f->getName().equals("MPI_Irecv")) {
			mpi_Irecv = f;
			conflicting_functions.insert(f);

			// Other MPI functions, that themselves may not yield another conflict
		} else if (f->getName().equals("MPI_Buffer_detach")) {
			mpi_buffer_detach = f;
			unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Test")) {
			mpi_test = f;
			unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Wait")) {
			mpi_wait = f;
			unimportant_functions.insert(f);
		} else if (f->getName().equals("MPI_Waitall")) {
			mpi_waitall = f;
			unimportant_functions.insert(f);

			//self implemented Partitioned functions
			// search for either the normal name (C mode) or the mangled name (C++ mode)
		} else if (f->getName().equals("MPIX_Psend_init")|| f->getName().equals("_Z15MPIX_Psend_initPvixiiiiiP12MPIX_Request")) {
			mpix_Psend_init = f;
		} else if (f->getName().equals("MPIX_Precv_init")|| f->getName().equals("_Z15MPIX_Precv_initPvixiiiiiP12MPIX_Request")) {
			mpix_Precv_init = f;
		} else if (f->getName().equals("MPIX_Start")|| f->getName().equals("_Z10MPIX_StartP12MPIX_Request")) {
			mpix_Start = f;
		} else if (f->getName().equals("MPIX_Wait")|| f->getName().equals("_Z9MPIX_WaitP12MPIX_RequestP10MPI_Status")) {
			mpix_Wait = f;

		} else if (f->getName().equals("MPIX_Pready")|| f->getName().equals("_Z11MPIX_PreadyiP12MPIX_Request")) {
			mpix_Pready = f;
		} else if (f->getName().equals("MPIX_Pready_range")|| f->getName().equals("_Z17MPIX_Pready_rangeiiP12MPIX_Request")) {
			mpix_Pready_range = f;
		} else if (f->getName().equals("MPIX_Request_free")
				|| f->getName().equals("_Z17MPIX_Request_freeP12MPIX_Request")) {
			mpix_Request_free = f;

			// library funcs to handle the partitioning
		} else if (f->getName().equals("partition_sending_op")
				|| f->getName().equals(
						"_Z20partition_sending_opPvxiiiiP12MPIX_Requestlllllll")) {
			partition_sending_op = f;

		} else if (f->getName().equals("signoff_partitions_after_loop_iter")
				|| f->getName().equals(
						"_Z34signoff_partitions_after_loop_iterP12MPIX_Requestll")) {
			signoff_partitions_after_loop_iter = f;
		}
	}

	//check if all defined functions are present
	bool all_present = (mpix_Psend_init != nullptr)
			&& (mpix_Precv_init != nullptr)
			&& (mpix_Start != nullptr) && (mpix_Wait != nullptr)
			&& (mpix_Pready != nullptr)
			&& (mpix_Pready_range != nullptr)
			&& (mpix_Request_free != nullptr);
	assert(
			partition_sending_op != nullptr
					&& signoff_partitions_after_loop_iter != nullptr);

	assert(
			all_present
					&& "Could not find all operations for Partitioned communication to be defined");

	// get type of mpix request for allocation of needed requests
	PointerType *pointer_t = dyn_cast<PointerType>(
			mpix_Request_free->getFunctionType()->getParamType(0));
	mpix_request_type = pointer_t->getElementType();
	assert(mpix_request_type);
}

bool MpiFunctions::is_mpi_used() {

	if (mpi_init != nullptr) {
		return mpi_init->getNumUses() > 0;
	} else {
		return false;
	}
}

bool MpiFunctions::is_send_function(llvm::Function *f) {
	assert(f != nullptr);
	return f == mpi_send || f == mpi_Bsend
			|| f == mpi_Ssend || f == mpi_Rsend
			|| f == mpi_Isend || f == mpi_Ibsend
			|| f == mpi_Irsend || f == mpi_Issend
			|| f == mpi_Sendrecv;
}

bool MpiFunctions::is_recv_function(llvm::Function *f) {
	assert(f != nullptr);
	return f == mpi_recv || f == mpi_Irecv
			|| f == mpi_Sendrecv;
}
