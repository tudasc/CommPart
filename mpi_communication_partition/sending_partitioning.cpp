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

#include "sending_partitioning.h"
#include "debug.h"
#include "helper.h"
#include "analysis_results.h"
#include "Openmp_region.h"
#include "insert_changes.h"
#include "mpi_functions.h"

#include "llvm/IR/Instruction.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/CFG.h"

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

std::vector<CallBase *> get_matching_waitall(AllocaInst *request_array,
                                             ConstantInt *index) {
  std::vector<CallBase *> result;

  for (auto u : request_array->users()) {
    if (auto *call = dyn_cast<CallBase>(u)) {
      if (call->getCalledFunction() == mpi_func->mpi_wait && index->isZero()) {
        // may use wait like this
        result.push_back(call);
      }
      if (call->getCalledFunction() == mpi_func->mpi_waitall) {
        if (is_waitall_matching(ConstantInt::get(index->getType(), 0), index,
                                call)) {
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
              if (call->getCalledFunction() == mpi_func->mpi_wait &&
                  index == index_in_array) {
                // may use wait like this
                result.push_back(call);
              }
              if (call->getCalledFunction() == mpi_func->mpi_waitall) {
                if (is_waitall_matching(index_in_array, index, call)) {
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

std::vector<CallBase *> get_corresponding_wait(CallBase *call) {

  // errs() << "Analyzing scope of \n";
  // call->dump();

  std::vector<CallBase *> result;
  unsigned int req_arg_pos = 6;
  if (call->getCalledFunction() == mpi_func->mpi_Ibarrier) {
    assert(call->getNumArgOperands() == 2);
    req_arg_pos = 1;
  } else {
    assert(call->getCalledFunction() == mpi_func->mpi_Isend ||
           call->getCalledFunction() == mpi_func->mpi_Ibsend ||
           call->getCalledFunction() == mpi_func->mpi_Issend ||
           call->getCalledFunction() == mpi_func->mpi_Irsend ||
           call->getCalledFunction() == mpi_func->mpi_Irecv ||
           call->getCalledFunction() == mpi_func->mpi_Iallreduce);
    assert(call->getNumArgOperands() == 7);
  }

  Value *req = call->getArgOperand(req_arg_pos);

  // req->dump();
  if (auto *alloc = dyn_cast<AllocaInst>(req)) {
    for (auto *user : alloc->users()) {
      if (auto *other_call = dyn_cast<CallBase>(user)) {
        if (other_call->getCalledFunction() == mpi_func->mpi_wait) {
          assert(other_call->getNumArgOperands() == 2);
          assert(other_call->getArgOperand(0) == req &&
                 "First arg of MPi wait is MPI_Request");
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
      if (auto *req_array = dyn_cast<AllocaInst>(gep->getPointerOperand())) {
        if (gep->getNumIndices() == 2 && gep->hasAllConstantIndices()) {

          auto *index_it = gep->idx_begin();
          ConstantInt *i0 = dyn_cast<ConstantInt>(&*index_it);
          index_it++;
          ConstantInt *index_in_array = dyn_cast<ConstantInt>(&*index_it);
          if (i0->isZero()) {

            auto temp = get_matching_waitall(req_array, index_in_array);
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
      errs() << "Strange, out of bounds getelemptr instruction should not "
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
      assert(finalize_call->getCalledFunction() == mpi_func->mpi_finalize);
      result.push_back(finalize_call);
    }
  }

  return result;
}


inline bool should_be_excluded(User *u,
		const std::vector<Instruction*> exclude_instructions) {

	return (std::find(exclude_instructions.begin(), exclude_instructions.end(),
			u) != exclude_instructions.end());
}

//TODO
// if no nocapture is given in a call, we should not move around this instructions anywhere!
// we cann not be shure what other functions do
//TODO we neet to check this nocapture attribute for ALL function call, even if we previously found another store as last point of modification!
bool should_function_call_be_considered_modification(Value *ptr,
		CallInst *call) {
	assert(
			call->hasArgument(ptr)
					&& "You are calling an MPI sendbuffer?\n I refuse to analyze this dark pointer magic!\n");

	unsigned operand_position = 0;
	while (call->getArgOperand(operand_position) != ptr) {
		++operand_position;
	}		// will terminate, as assert above secured that

	if (operand_position
			>= call->getCalledFunction()->getFunctionType()->getNumParams()) {
		assert(call->getCalledFunction()->getFunctionType()->isVarArg());
		operand_position =
				call->getCalledFunction()->getFunctionType()->getNumParams()
						- 1;
		// it is one of the VarArgs
	}

	auto *ptr_argument = call->getCalledFunction()->getArg(operand_position);

	if (call->getCalledFunction()->getName().equals("__kmpc_fork_call")) {

		Microtask *parallel_region = new Microtask(call);

		auto *buffer_ptr = parallel_region->get_value_in_mikrotask(ptr);

		if (!buffer_ptr->hasNoCaptureAttr()) {
			assert(
					false
							&& "Function call captures the send buffer, Abort analysis\n");
			return true;
		}

		if (buffer_ptr->hasNoCaptureAttr()
				&& buffer_ptr->hasAttribute(Attribute::ReadOnly)) {
			// readonly and nocapture: nothing need to be done

			//TODO actually it is allowed to load from ptr and then use a GEP to load it
			// in some cases a firstprivate message should be used!!
			errs() << "buffer has readonly\n";

			return false;
		} else {
			return true;
		}
	} else if (call->getCalledFunction() == mpi_func->mpi_send) {
		//TODO implement
		//
		return true;
	} else if (call->getCalledFunction() == mpi_func->mpi_recv) {
		// sending never does writing
		return false;
	} else {		// no known MPI or openmp func
		if (ptr_argument->hasNoCaptureAttr()) {
			if (call->getCalledFunction()->onlyReadsMemory()) {
				//readonly is ok --> nothing to do
				return false;
			} else {
				unsigned operand_position = 0;
				while (call->getArgOperand(operand_position) != ptr) {
					operand_position++;
				}

				if (ptr_argument->hasAttribute(Attribute::ReadOnly)) {
					//readonly is ok --> nothing to do#
					return false;
				} else {
					// function may write
					return true;
				}
			}
		} else {
			// no nocapture: function captures the pointer ==> others may access it
			// we cannot do any further analysis
			errs() << "Function " << call->getCalledFunction()->getName()
					<< " captures the send buffer pointer\n No further analysis possible\n";
			//assert(false); ??
			return true;				// no may write
		}
	}

	call->dump();
	assert(false && "SHOULD NEVER REACH THIS");
	return true;

}

// if search_before == nullptr: return all usages of ptr not in exclude_instruction
std::vector<Instruction*> get_ptr_usages(Value *ptr, Instruction *search_before,
		const std::vector<Instruction*> exclude_instructions) {
	assert(ptr != nullptr);
	assert(search_before != nullptr);
	// gather all uses of this buffer
	LoopInfo *linfo = analysis_results->getLoopInfo(
			search_before->getParent()->getParent());
	DominatorTree *dt = analysis_results->getDomTree(
			search_before->getParent()->getParent());

	std::vector<Instruction*> to_analyze;
	for (auto *u : ptr->users()) {
		if (!should_be_excluded(u, exclude_instructions)) {

			if (auto *inst = dyn_cast<Instruction>(u)) {

				if (search_before != nullptr) {
					if (llvm::isPotentiallyReachable(inst, search_before,
							nullptr, dt, linfo)) {
						// only then it may have an effect
						to_analyze.push_back(inst);
					}
				} else {

					to_analyze.push_back(inst);
				}

			}
		}
	}
	return to_analyze;
}

//TODO refactoring so that default values are set instead of voerloading?
Instruction* search_for_pointer_modification(Value *ptr,
		std::vector<Instruction*> to_analyze, Instruction *search_before,
		const std::vector<Instruction*> exclude_instructions);

Instruction* search_for_pointer_modification(Value *ptr,
		std::vector<Instruction*> to_analyze,
		const std::vector<Instruction*> exclude_instructions) {
	return search_for_pointer_modification(ptr, to_analyze, nullptr,
			exclude_instructions);
}

Instruction* search_for_pointer_modification(Value *ptr,
		Instruction *search_before,
		const std::vector<Instruction*> exclude_instructions) {
	std::vector<Instruction*> to_analyze;
	return search_for_pointer_modification(ptr, to_analyze, search_before,
			exclude_instructions);
}

// returns most latest ptr modification
// or nullptr if no found
// to_analyze may be empty than use all usages of ptr before search_before
// search_before may be nullptr
Instruction* search_for_pointer_modification(Value *ptr,
		std::vector<Instruction*> to_analyze, Instruction *search_before,
		const std::vector<Instruction*> exclude_instructions) {

	if (to_analyze.size() == 0) {
		to_analyze = get_ptr_usages(ptr, search_before, exclude_instructions);
	}
	// still no usages:
	if (to_analyze.size() == 0) {
		return nullptr;
	}

	Instruction *current_instruction = get_last_instruction(to_analyze);

	std::vector<Instruction*> modification_points;

	while (current_instruction != nullptr) {

		// otherwise it should not be included in to_analyze
		assert(
				std::find(exclude_instructions.begin(),
						exclude_instructions.end(), current_instruction)
						== exclude_instructions.end());

		if (auto *store = dyn_cast<StoreInst>(current_instruction)) {
			if (ptr == store->getValueOperand()) {
				// add all uses of this stores Pointer operand "taint" this pointer and treat it as the ptr itself
				// this will lead to false positives
				//but there are not much meaningful uses of this anyway
				modification_points.push_back(
						get_latest_modification_of_pointer(
								store->getPointerOperand(), search_before,
								exclude_instructions));
			} else {
				assert(ptr == store->getPointerOperand());
				modification_points.push_back(store);
			}

		} else if (auto *call = dyn_cast<CallInst>(current_instruction)) {
			if (should_function_call_be_considered_modification(ptr, call)) {
				modification_points.push_back(call);
			}
		} else if (auto *load = dyn_cast<LoadInst>(current_instruction)) {
			assert(load->getPointerOperand() == ptr);
			// loading from ptr does not modify--> nothing to do

		} else if (auto *gep = dyn_cast<GetElementPtrInst>(
				current_instruction)) {
			modification_points.push_back(
					search_for_pointer_modification(gep, search_before,
							exclude_instructions));

		} else if (auto *unary = dyn_cast<UnaryInstruction>(
				current_instruction)) {
			// such as cast
			assert(
					unary->getType()->isPointerTy()
							&& "Cast pointer to non pointer. Analysis of this is not supported ");
			// get a possible modification point
			modification_points.push_back(
					search_for_pointer_modification(unary, search_before,
							exclude_instructions));

		} else {
			errs()
					<< "Support for the analysis of this instruction is not implemented yet\n";
			current_instruction->print(errs());
			errs() << "\n";
			modification_points.push_back(current_instruction);
		}

		//TODO handle other instructions such as

		// remove erase the analyzed instruction
		to_analyze.erase(
				std::remove(to_analyze.begin(), to_analyze.end(),
						current_instruction), to_analyze.end());

		if (to_analyze.size() > 0) {
			current_instruction = get_last_instruction(to_analyze);
		} else {
			current_instruction = nullptr;
			// nothing more to do, analyzed all possible modification points

			// there may be nullptrs in it
			modification_points.erase(
					std::remove(modification_points.begin(),
							modification_points.end(), nullptr),
					modification_points.end());
			if (modification_points.size() == 0) {
				return nullptr;
			} else {
				return get_last_instruction(modification_points);
			}
		}

	}
	errs()
			<< "No pointer modification detected so far, should return earlier!\n To analyze:\n";
	for (auto *i : to_analyze) {
		i->dump();
	}

	assert(false && "SHOULD NOT REACH THIS");
	return nullptr;

}

// true if the location pointed by ptr can be considered loop invariant if exclude_instructions are ignored
// false if unshure
bool is_location_loop_invariant(Value *ptr, Loop *loop,
		const std::vector<Instruction*> exclude_instructions) {

	if (!loop->isLoopInvariant(ptr)) {
		// ptr not loopp invariant: analysis makes no sense
		return false;
	}
	std::vector<Instruction*> to_analyze;
	for (auto *u : ptr->users()) {
		if (auto *inst = dyn_cast<Instruction>(u)) {
			if ((!(std::find(exclude_instructions.begin(),
					exclude_instructions.end(), inst)
					!= exclude_instructions.end())) && loop->contains(inst)) {
				// not ignored and in loop
				to_analyze.push_back(inst);

			}
		}

	}

// if nullptr then no modification found and retrun true
	return (nullptr
			== search_for_pointer_modification(ptr, to_analyze, nullptr,
					exclude_instructions));
}

// will yield the latest modification = write to ptr
// before the instruction in search_before, but exclude exclude_instructions --> e.g. instr that where already handled
// if search_before is in a loop, will move out of the loop, if determined the load is loop invariant if exclude_instructions are ignored
Instruction* get_latest_modification_of_pointer(Value *ptr,
		Instruction *search_before,
		const std::vector<Instruction*> exclude_instructions) {

	assert(ptr != nullptr);
	assert(search_before != nullptr);

	std::vector<Instruction*> to_analyze = get_ptr_usages(ptr, search_before,
			exclude_instructions);

	auto *modification_point = search_for_pointer_modification(ptr, to_analyze,
			search_before, exclude_instructions);

	if (modification_point == nullptr) {

		if (auto *ptr_as_inst = dyn_cast<Instruction>(ptr)) {
			//Pointer is the result of an instruction (such as GEP)
			// need to trace uses of the original pointer
			if (auto *gep = dyn_cast<GetElementPtrInst>(ptr)) {
				return get_latest_modification_of_pointer(
						gep->getPointerOperand(), search_before,
						exclude_instructions);
			} else if (auto *unary = dyn_cast<UnaryInstruction>(ptr)) {
				return get_latest_modification_of_pointer(unary->getOperand(0),
						search_before, exclude_instructions);
			} else {
				errs() << "Analysis of this is not supported Yet\n";
				ptr_as_inst->dump();
			}
		}

		// first instruction of function if not modified within the function
		return search_before->getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();

	}
	LoopInfo *linfo = analysis_results->getLoopInfo(
			search_before->getParent()->getParent());

	if (auto *loop = linfo->getLoopFor(search_before->getParent())) {
		if (!is_location_loop_invariant(ptr, loop, exclude_instructions)) {
			if (!loop->contains(modification_point)) {
				// meaning that modification occurs after search before in the loop
				//TODO can we move the instruction to the beginning of a loop body?

				return search_before->getPrevNode();//to be shure: suggest no movement
			}
		}
	}

	return modification_point;

}

//TODO change discover of buffer ptr
// buffer ptr may be the higher order ptr (before GEP)
// send might be done on ptr resulting from GEP

// returns true if fork call is handled
// false if thread doesn't write to buffer and nothing need to be done

//TODO call_handle_modification at all exits of function?
// or refactor so that return means something different
bool handle_fork_call(Microtask *parallel_region, CallInst *send_call) {

	auto *buffer_ptr_in_main = send_call->getArgOperand(0);

	auto *buffer_ptr = parallel_region->get_value_in_mikrotask(
			buffer_ptr_in_main);

	if (buffer_ptr->hasNoCaptureAttr()
			&& buffer_ptr->hasAttribute(Attribute::ReadOnly)) {
		// readonly and nocapture: nothing need to be done
		return false;
	}

	errs() << "Handle Fork Call\n";

	if (parallel_region->get_parallel_for()) {
		// we have to build a list of all accesses to the buffer

		auto store_list = get_instruction_in_function<StoreInst>(
				parallel_region->get_function());

		auto call_list = get_instruction_in_function<CallBase>(
				parallel_region->get_function());

		// make shure no called function writes the msg buffer (such calls should be inlined beforehand)

		//TODO check for this AA bug
		auto *AA = analysis_results->getAAResults(
				parallel_region->get_function());

		for (auto *call : call_list) {

			// check if call is openmp RTL call
			if (!call->getCalledFunction()->getName().startswith("__kmpc_")) {

				//TODO do i need to handle MPi calls seperately?

				for (auto &a : call->args()) {

					if (auto *arg = dyn_cast<Value>(a)) {

						if (arg->getType()->isPointerTy()) {

							if (!AA->isNoAlias(buffer_ptr, arg)) {

								// may alias or must alias
								//TODO Problem ptr is a function arg and may therefore alias with everything in function!

								//TODO check if nocapture and readonly in tgt function

								call->dump();
								errs()
										<< "Found call with ptr that may alias, analysis is not detailed here\n";

								if (true) {
									// found function that accesses the buffer: threat the whole parallel as store
									//TODO maybe force inlining to allow further analysis?
									handle_modification_location(send_call,
											parallel_region->get_fork_call());
									return true;
								}
							}
						}
					}
				}
			}
		}
		// checked all call instructions

		// need to check all store instructions
		// first we need to check if there is a store PAST the for loop accessing the ptr
		auto *linfo = analysis_results->getLoopInfo(
				parallel_region->get_function());
		// we checed for the presence of the openmp loobs before
		assert(!linfo->empty());

		auto *DT = analysis_results->getDomTree(
				parallel_region->get_function());
		auto *PDT = analysis_results->getPostDomTree(
				parallel_region->get_function());

		//TODO implement analysis of multiple for loops??
		auto *loop_exit = parallel_region->get_parallel_for()->fini;

		AA = analysis_results->getAAResults(parallel_region->get_function());

		// filter out all stores that can not alias
		store_list.erase(
				std::remove_if(store_list.begin(), store_list.end(),
						[AA, buffer_ptr](llvm::StoreInst *s) {
							return AA->isNoAlias(buffer_ptr,
									s->getPointerOperand());
						}),store_list.end());

		// check if all remaining are within (or before the loop)
		bool are_all_stores_before_loop_finish = std::all_of(store_list.begin(),
				store_list.end(), [PDT, loop_exit](llvm::StoreInst *s) {

					return PDT->dominates(loop_exit, s);

				});

		//errs() << "All before loop exit?"
		//		<< are_all_stores_before_loop_finish << "\n";
		if (!are_all_stores_before_loop_finish) {
			//cannot determine a partitioning if access is outside the loop
			handle_modification_location(send_call,
					parallel_region->get_fork_call());
			errs() << "Stores after the loop: No partitioning possible\n";
			return true;
		}
		//TODO do we need to consider stores before the loop?

		// TODO is assertion correct --> was it actually checked before?
		assert(!store_list.empty());

/// now we need to get min and maximum memory access pattern for every store in the loop

		auto *SE = analysis_results->getSE(parallel_region->get_function());

		const SCEV *min = SE->getSCEV(store_list[0]->getPointerOperand());
		const SCEV *max = SE->getSCEV(store_list[0]->getPointerOperand());

		// skip first
		for (auto s = ++store_list.begin(); s != store_list.end(); ++s) {

			auto *candidate = SE->getSCEV((*s)->getPointerOperand());

			candidate->dump();

			if (SE->isKnownPredicate(CmpInst::Predicate::ICMP_SLE, candidate,
					min)) {
				min = candidate;
			} else {
				if (!SE->isKnownPredicate(CmpInst::Predicate::ICMP_SLE,
						candidate, min))
					errs() << "Error analyzing the memory access pattern\n";
				// we cant do anything
				return true;
			}
			if (SE->isKnownPredicate(CmpInst::Predicate::ICMP_SLE, max,
					candidate)) {
				min = candidate;
			} else {
				if (!SE->isKnownPredicate(CmpInst::Predicate::ICMP_SLE,
						candidate, max))
					errs() << "Error analyzing the memory access pattern\n";
				// we cant do anything
				return true;

			}
		}

		auto *LI = analysis_results->getLoopInfo(
				parallel_region->get_function());

		//TODO refactoring!
		// this shcould be part of microtask?
		// the for init call is before the loop preheader
		auto *preheader =
				parallel_region->get_parallel_for()->init->getParent()->getNextNode();

		auto *loop = LI->getLoopFor(preheader->getNextNode());

		assert(loop != nullptr);
		//loop->print(errs()

		if (auto *min_correct_form = dyn_cast<SCEVAddRecExpr>(min)) {
			if (auto *max_correct_form = dyn_cast<SCEVAddRecExpr>(max)) {
				if (min_correct_form->isAffine()
						&& max_correct_form->isAffine()) {

					insert_partitioning(parallel_region, send_call,
							min_correct_form, max_correct_form);

					return true;
				}

			}
		}

		//TODO check if values are constant?

		errs() << "Error analyzing the memory access pattern\n";
		min->dump();
		max->dump();

		// we cant do anything
		return true;

	} else {
		// no parallel for
		// TODO handle task pragma?

		// we cannot split the buffer accesses among the different threads, need the handle this fork call as a write function call

		handle_modification_location(send_call,
				parallel_region->get_fork_call());
		return true;
	}

	return false;

}

// find usage of sending buffer
// if usage == openmp fork call
// analyze the parallel region to find if partitioning is possible

// return ture if change was made to the IR
bool handle_send_call(CallInst *send_call) {
//if using e.g. the adress of MPI send as buffer the user is dumb anyway
	assert(send_call->getCalledFunction() == mpi_func->mpi_send);
	Debug(
			errs() << "Handle Send call:\n";
			send_call->print(errs());
			errs() << "\n";)

	auto *buffer_ptr = send_call->getArgOperand(0);

	std::vector<Instruction*> ignore = { send_call };

	Instruction *latest_modification = get_latest_modification_of_pointer(
			buffer_ptr, send_call, ignore);

	if (auto *call = dyn_cast<CallInst>(latest_modification)) {
		if (call->getCalledFunction()->getName().equals("__kmpc_fork_call")) {
			Microtask *parallel_region = new Microtask(call);
			return handle_fork_call(parallel_region, send_call);
		} else {
			return handle_modification_location(send_call, latest_modification);
		}
	} else {

		return handle_modification_location(send_call, latest_modification);

	}
}

