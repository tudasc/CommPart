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
#include "mpi_analysis.h"
#include "Openmp_region.h"
#include "insert_changes.h"
#include "mpi_functions.h"

#include "llvm/IR/Instruction.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/CFG.h"

using namespace llvm;

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

			errs()
					<< "buffer has readonly Consider using firstprivate for the buffer pointer\n";
			//TODO actually it is allowed to load from ptr and then use a GEP to load it
			// in such cases a firstprivate message should be used!!

			return true;
		} else {
			return true;
		}
	} else if (mpi_func->is_send_function(call->getCalledFunction())) {
		// sending never does writing
		//
		return false;
	} else if (mpi_func->is_recv_function(call->getCalledFunction())) {
		// recv will write
		return true;
	} else if (call->getCalledFunction() == mpi_func->partition_sending_op) {
		// if an operation was replaced with a partitioned operation,
		// we have previously analyzed that there is no conflicting operation
		// meaning we can safetly ignore the already partitioned operations
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
					// this may not be true!!
					//TODO a more detailed analysis?
					return true;
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
			return true;			// no may write
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

	// gather all uses of this buffer

	std::vector<Instruction*> to_analyze;
	for (auto *u : ptr->users()) {
		if (!should_be_excluded(u, exclude_instructions)) {

			if (auto *inst = dyn_cast<Instruction>(u)) {

				if (search_before != nullptr) {
					LoopInfo *linfo = analysis_results->getLoopInfo(
							inst->getFunction());
					DominatorTree *dt = analysis_results->getDomTree(
							inst->getFunction());
					if (llvm::isPotentiallyReachable(inst, search_before,
							nullptr, dt, linfo)) {
						// only then it may have an effect

						bool exclude = false;

						if (auto *loop = linfo->getLoopFor(
								search_before->getParent())) {
							// exclude if it is inside loop but after send
							if (loop == linfo->getLoopFor(inst->getParent())) {

								if (search_before
										== get_first_instruction(inst,
												search_before)) {
									exclude = true;
								}

							}

						}
						if (!exclude) {

							to_analyze.push_back(inst);
						}
					}
				} else {				// search_before==nullptr

					to_analyze.push_back(inst);
				}

			}
		}
	}
	return to_analyze;
}

// true if value ore something computed from value goes into a partition send call
bool is_value_passed_into_partition_call(Value *v) {

	for (auto *u : v->users())
		if (auto *call = dyn_cast<CallInst>(u)) {
			if (call->getCalledFunction() == mpi_func->partition_sending_op) {
				return true;
			} else {
				// passing it into another func is not supported
				return false;
			}
		} else {
			if (is_value_passed_into_partition_call(u))
				return true;
		}
// nothing found -- this also ends recursion if v.users.size==0
	return false;
}

// give all store operations that write to ptr or a location derived from ptr
void get_all_stores_resulting_from_ptr(Value *ptr,
		std::vector<StoreInst*> &result) {

	for (auto *u : ptr->users()) {
		if (auto *store = dyn_cast<StoreInst>(u)) {
			if (store->getPointerOperand() == ptr) {
				result.push_back(store);
			}
		} else if (auto *load = dyn_cast<LoadInst>(u)) {
			// no need to trace loaded value
		} else if (auto *call = dyn_cast<CallBase>(u)) {
			//TODO check if call is readonly and assert fail if not??
		} else if (auto *i = dyn_cast<Instruction>(u)) {
			// insert all resulting stores form derived into result vector
			get_all_stores_resulting_from_ptr(i, result);
		} else {
			// should always be an instruction
			assert(false && "Pointer is used in non-Indtruction?");
		}
	}

}

//TODO refactoring so that default values are set instead of overloading?
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

			if (unary->getType()->isPointerTy()) {
				// get a possible modification point
				modification_points.push_back(
						search_for_pointer_modification(unary, search_before,
								exclude_instructions));
			} else {

				// if this goes into a partition_sending_op_call it is OK
				if (!is_value_passed_into_partition_call(unary)) {
					// otherwise
					unary->dump();
					assert(
							unary->getType()->isPointerTy()
									&& "Pointer is casted to non-pointer. Analysis of this is not supported yet");
				}

			}

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
// if search before is in a loop: will not consider operations after search_before
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

	if (parallel_region->get_function()->hasFnAttribute(Attribute::ReadOnly)) {
		// readonly nothing need to be done
		return false;
	}

	auto *buffer_ptr_in_main = send_call->getArgOperand(0);

	std::vector<Value*> buffer_ptr_aliases_in_main;

//TODO check for this AA bug?
	auto *AA = analysis_results->getAAResults(
			parallel_region->get_fork_call()->getFunction());

	for (Value *fork_call_arg : parallel_region->get_fork_call()->arg_operands()) {

		if (fork_call_arg->getType()->isPointerTy()) {
			if (!AA->isNoAlias(buffer_ptr_in_main, fork_call_arg)) {
				buffer_ptr_aliases_in_main.push_back(fork_call_arg);

				//fork_call_arg->dump();
			}

		}

	}

	if (buffer_ptr_aliases_in_main.size() == 0) {
		// not actually getting a buffer ptr
		assert(false);				// this function should not be called then
		return false;
	}

	std::vector<Value*> buffer_ptr_aliases_in_parallel;
	buffer_ptr_aliases_in_parallel.reserve(buffer_ptr_aliases_in_main.size());

	std::transform(buffer_ptr_aliases_in_main.begin(),
			buffer_ptr_aliases_in_main.end(),
			std::back_inserter(buffer_ptr_aliases_in_parallel),
			[parallel_region](auto *v) {
				return parallel_region->get_value_in_mikrotask(v);
			});
// all values needs to be defined
	Debug(
			for(auto*p:buffer_ptr_aliases_in_parallel) {assert(p!=nullptr);}
	)

	errs() << "Handle Fork Call\n";

	if (parallel_region->get_parallel_for()) {
		// we have to build a list of all accesses to the buffer

		auto call_list = get_instruction_in_function<CallBase>(
				parallel_region->get_function());

		// make shure no called function writes the msg buffer (such calls should be inlined beforehand)

		for (auto *call : call_list) {

			// check if call is openmp RTL call
			if (!call->getCalledFunction()->getName().startswith("__kmpc_")) {

				// do i need to handle MPi calls seperately?
				// there should not be MPI inside openmp due to multithreaded performance issues of MPI
				// but i need to exclude the already made changes:
				if (!(call->getCalledFunction()
						== mpi_func->signoff_partitions_after_loop_iter)) {

					for (auto &a : call->args()) {

						if (auto *arg = dyn_cast<Value>(a)) {

							if (arg->getType()->isPointerTy()) {

								if (at_least_one_may_alias(
										parallel_region->get_function(), arg,
										buffer_ptr_aliases_in_parallel)) {

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
		}
		// checked all call instructions

		std::vector<StoreInst*> store_list;

		for (auto *p : buffer_ptr_aliases_in_parallel) {

			get_all_stores_resulting_from_ptr(p, store_list);
		}

		// need to check all store instructions
		// first we need to check if there is a store PAST the for loop accessing the ptr
		auto *linfo = analysis_results->getLoopInfo(
				parallel_region->get_function());
		// we checed for the presence of the openmp loops before
		assert(!linfo->empty());

		auto *DT = analysis_results->getDomTree(
				parallel_region->get_function());
		auto *PDT = analysis_results->getPostDomTree(
				parallel_region->get_function());

		//TODO implement analysis of multiple for loops??
		auto *loop_exit = parallel_region->get_parallel_for()->fini;

		// check if all stores are within (or before the loop)
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

			(*s)->dump();
			candidate->dump();
			//TODO Problem: if location is fixed (loop invariant) we can not compare it against loop-chainging pattern (as we do not know allocation layout beforehand)
			// min and max is hard to define in this case

			//TODO why is this important anyway, the single shared var should never alias with pointer!

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

		auto *loop = parallel_region->get_for_loop();
		assert(loop != nullptr);
		//loop->print(errs());

		//{{{(8 + (8 * (1 + (sext i32 %14 to i64))<nsw> * %19) + %3),+,
		//(8 * (sext i32 %15 to i64) * %19)}<%omp.inner.for.cond.preheader>,+,
			//(8 * %19)<nsw>}<%omp.inner.for.body>,+,8}<nsw><%for.body>

		//TODO Problem: the current impl will only use min
		// but we need max of inner loop


		// loop->getBounds->getFinalIVValue()
		//min = start
		// max = start + end_value-begin_value * step
		// must be computable (at least recusively)
		// if not computalbe it cannot be an openmp loop
		// max value if inner loop must be invariant regarding uter loop for our analysis


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
	assert(
			send_call->getCalledFunction() == mpi_func->mpi_send
					|| send_call->getCalledFunction() == mpi_func->mpi_Isend);
	Debug(
			errs() << "Handle Send call:\n";
			send_call->print(errs());
			errs() << "\n";)

	auto *buffer_ptr = send_call->getArgOperand(0);

	std::vector<CallBase*> overlapping = find_overlapping_operations(send_call);

// convert to instruction*
	std::vector<Instruction*> ignore;
	ignore.reserve(overlapping.size() + 1);
	std::move(overlapping.begin(), overlapping.end(),
			std::back_inserter(ignore));
// and add the send call itself
	ignore.push_back(send_call);

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

