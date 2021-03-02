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

#include "mpi_analysis.h"
#include "mpi_functions.h"
#include "debug.h"
#include "helper.h"

using namespace llvm;

bool is_waitall_matching(ConstantInt *begin_index, ConstantInt *match_index,
		CallBase *call) {
	assert(begin_index->getType() == match_index->getType());
	assert(call->getCalledFunction() == mpi_func->mpi_waitall);
	assert(call->getNumArgOperands() == 3);

	Debug(call->dump(); errs() << "Is this waitall matching?";);

	if (auto *count = dyn_cast<ConstantInt>(call->getArgOperand(0))) {

		auto begin = begin_index->getSExtValue();
		auto match = match_index->getSExtValue();
		auto num_req = count->getSExtValue();

		if (begin + num_req > match && match >= begin) {
			// proven, that this request is part of the requests waited for by the
			// call
			Debug(errs() << "  TRUE\n";);
			return true;
		}
	}

	// could not prove true
	Debug(errs() << "  FALSE\n";);
	return false;
}

std::vector<CallBase*> get_matching_waitall(AllocaInst *request_array,
		ConstantInt *index) {
	std::vector<CallBase*> result;

	for (auto u : request_array->users()) {
		if (auto *call = dyn_cast<CallBase>(u)) {
			if (call->getCalledFunction() == mpi_func->mpi_wait
					&& index->isZero()) {
				// may use wait like this
				result.push_back(call);
			}
			if (call->getCalledFunction() == mpi_func->mpi_waitall) {
				if (is_waitall_matching(ConstantInt::get(index->getType(), 0),
						index, call)) {
					result.push_back(call);
				}
			}
		} else if (auto *gep = dyn_cast<GetElementPtrInst>(u)) {
			if (gep->getNumIndices() == 2 && gep->hasAllConstantIndices()) {
				auto *index_it = gep->idx_begin();
				ConstantInt *i0 = dyn_cast<ConstantInt>(&*index_it);
				index_it++;
				ConstantInt *index_in_array = dyn_cast<ConstantInt>(&*index_it);
				if (i0->isZero()) {

					for (auto u2 : gep->users()) {
						if (auto *call = dyn_cast<CallBase>(u2)) {
							if (call->getCalledFunction() == mpi_func->mpi_wait
									&& index == index_in_array) {
								// may use wait like this
								result.push_back(call);
							}
							if (call->getCalledFunction()
									== mpi_func->mpi_waitall) {
								if (is_waitall_matching(index_in_array, index,
										call)) {
									result.push_back(call);
								}
							}
						}
					}

				} // end if index0 == 0
			}   // end if gep has simple structure
		}
	}

	return result;
}

std::vector<CallBase*> get_corresponding_wait(CallBase *call) {

	// errs() << "Analyzing scope of \n";
	// call->dump();

	std::vector<CallBase*> result;
	unsigned int req_arg_pos = 6;
	if (call->getCalledFunction() == mpi_func->mpi_Ibarrier) {
		assert(call->getNumArgOperands() == 2);
		req_arg_pos = 1;
	} else {
		assert(
				call->getCalledFunction() == mpi_func->mpi_Isend
						|| call->getCalledFunction() == mpi_func->mpi_Ibsend
						|| call->getCalledFunction() == mpi_func->mpi_Issend
						|| call->getCalledFunction() == mpi_func->mpi_Irsend
						|| call->getCalledFunction() == mpi_func->mpi_Irecv
						|| call->getCalledFunction()
								== mpi_func->mpi_Iallreduce);
		assert(call->getNumArgOperands() == 7);
	}

	Value *req = call->getArgOperand(req_arg_pos);

	// req->dump();
	if (auto *alloc = dyn_cast<AllocaInst>(req)) {
		for (auto *user : alloc->users()) {
			if (auto *other_call = dyn_cast<CallBase>(user)) {
				if (other_call->getCalledFunction() == mpi_func->mpi_wait) {
					assert(other_call->getNumArgOperands() == 2);
					assert(
							other_call->getArgOperand(0) == req
									&& "First arg of MPi wait is MPI_Request");
					// found end of scope
					// errs() << "possible ending of scope here \n";
					// other_call->dump();
					result.push_back(other_call);
				}
			}
		}
	}
	// scope detection in basic waitall
	// ofc at some point of pointer arithmetic, we cannot follow it
	else if (auto *gep = dyn_cast<GetElementPtrInst>(req)) {
		if (gep->isInBounds()) {
			if (auto *req_array = dyn_cast<AllocaInst>(
					gep->getPointerOperand())) {
				if (gep->getNumIndices() == 2 && gep->hasAllConstantIndices()) {

					auto *index_it = gep->idx_begin();
					ConstantInt *i0 = dyn_cast<ConstantInt>(&*index_it);
					index_it++;
					ConstantInt *index_in_array = dyn_cast<ConstantInt>(
							&*index_it);
					if (i0->isZero()) {

						auto temp = get_matching_waitall(req_array,
								index_in_array);
						result.insert(result.end(), temp.begin(), temp.end());

					} // end it index0 == 0
				}   // end if gep has a simple structure
				else {
					// debug
					Debug(gep->dump();
							errs()
							<< "This structure is currently too complicated to analyze";);
				}
			}

		} else { // end if inbounds
			gep->dump();
			errs()
					<< "Strange, out of bounds getelemptr instruction should not "
							"happen in this case\n";
		}
	}

	if (result.empty()) {
		errs() << "could not determine scope of \n";
		call->dump();
		errs() << "Assuming it will finish at mpi_finalize.\n"
				<< "The Analysis result is still valid, although the chance of "
						"false positives is higher\n";
	}

	// mpi finalize will end all communication nontheles
	for (auto *user : mpi_func->mpi_finalize->users()) {
		if (auto *finalize_call = dyn_cast<CallBase>(user)) {
			assert(
					finalize_call->getCalledFunction()
							== mpi_func->mpi_finalize);
			result.push_back(finalize_call);
		}
	}

	return result;
}

//returns the call where the operation is completed locally
// e.g. the corresponding wait ot the ooperation itself for blocking communication
Instruction* get_local_completion_point(CallBase *mpi_call) {

	if (mpi_call->getCalledFunction() == mpi_func->mpi_Ibarrier
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Isend
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Ibsend
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Issend
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Irsend
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Irecv
			|| mpi_call->getCalledFunction() == mpi_func->mpi_Iallreduce) {
		auto possible_completion_points = get_corresponding_wait(mpi_call);

		return get_first_instruction(possible_completion_points);

	} else { // blocking operations
		assert(
				mpi_call->getCalledFunction() == mpi_func->mpi_send
						|| mpi_call->getCalledFunction() == mpi_func->mpi_recv);

		return mpi_call;
	}

}

//TODO if A overlaps B, B overlaps A -> caching the results might be possible to not build the result again and again
// maybe even an own analysis pass?
// returns operations that may locally overlap (will not include self)
std::vector<CallBase*> find_overlapping_operations(CallBase *mpi_call) {

	std::vector<CallBase*> result;
	auto *completion_point = get_local_completion_point(mpi_call);

	for (auto *mpi_function : mpi_func->get_used_send_and_recv_functions()) {
		for (auto *u : mpi_function->users()) {
			if (auto *other_call = dyn_cast<CallBase>(u)) {
				assert(other_call->getCalledFunction() == mpi_function);
				auto *other_completion_point = get_local_completion_point(
						other_call);

				if (other_completion_point
						== get_first_instruction(other_completion_point,
								mpi_call)
						|| completion_point
								== get_first_instruction(completion_point,
										other_call)) {
					// if one is finished before the other: no overlap

				} else {

					// if it is in between
					if (nullptr
							!= get_first_instruction(other_completion_point,
									mpi_call)
							|| nullptr
									!= get_first_instruction(completion_point,
											other_call)) {
						// if there is another defined order:
						// overlapping
						result.push_back(other_call);
					}else{
						// no defined order: can not prove overlapping
					}
				}

			}
		}
	}

	//TODO this dopes not capture the calls that start before the

	return result;

}
