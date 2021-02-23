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
	send_call->print(errs());
	errs() << "\n";

	LoopInfo *linfo = analysis_results->getLoopInfo(
			send_call->getParent()->getParent());
	DominatorTree *dt = analysis_results->getDomTree(
			send_call->getParent()->getParent());

	if (linfo->getLoopFor(send_call->getParent()) != nullptr) {
		errs() << "Send in loop is currently not supported\n";
		assert(false);
	}
	auto *buffer_ptr = send_call->getArgOperand(0);

	// gather all uses of this buffer
	std::vector<Instruction*> to_analyze;
	for (auto *u : buffer_ptr->users()) {
		if (u != send_call) {

			if (auto *inst = dyn_cast<Instruction>(u)) {

				if (llvm::isPotentiallyReachable(inst, send_call, nullptr, dt,
						linfo)) {
					// only then it may have an effect
					to_analyze.push_back(inst);
				}
			}
		}
	}

	//TODO need to refactor this!
	Instruction *to_handle = get_last_instruction(to_analyze);
	while (to_handle != nullptr) {

		//to_handle->print(errs());
		//errs() << "\n";
		if (auto *store = dyn_cast<StoreInst>(to_handle)) {
			if (buffer_ptr == store->getValueOperand()) {
				// add all uses of the Pointer operand "taint" this pointer and treat it as the msg buffer itself
				// this will lead to false positives
				//but there are not much meaningful uses of this anyway

				for (auto *u : store->getPointerOperand()->users()) {
					if (u != store) {
						if (auto *i = dyn_cast<Instruction>(u)) {
							to_analyze.push_back(i);
						}
					}
				}
			} else {
				assert(buffer_ptr == store->getPointerOperand());
				return handle_modification_location(send_call, to_handle);
			}

		} else if (auto *call = dyn_cast<CallInst>(to_handle)) {

			assert(
					call->hasArgument(buffer_ptr)
							&& "You arer calling an MPI sendbuffer?\n I refuse to analyze this dak pointer magic!\n");

			unsigned operand_position = 0;
			while (call->getArgOperand(operand_position) != buffer_ptr) {
				++operand_position;
			}		// will terminate, as assert above secured that

			if (operand_position
					>= call->getCalledFunction()->getFunctionType()->getNumParams()) {
				assert(
						call->getCalledFunction()->getFunctionType()->isVarArg());
				operand_position =
						call->getCalledFunction()->getFunctionType()->getNumParams()
								- 1;
				// it is one of the VarArgs
			}

			auto *ptr_argument = call->getCalledFunction()->getArg(
					operand_position);

			if (call->getCalledFunction()->getName().equals(
					"__kmpc_fork_call")) {

				Microtask *parallel_region = new Microtask(call);

				bool handled = handle_fork_call(parallel_region, send_call);
				if (handled) {
					//TODO refactro if output form handle_fork_call change
					return true;
				}

			} else {
				if (ptr_argument->hasNoCaptureAttr()) {
					if (call->getCalledFunction()->onlyReadsMemory()) {
						//readonly is ok --> nothing to do
					} else {
						unsigned operand_position = 0;
						while (call->getArgOperand(operand_position)
								!= buffer_ptr) {
							operand_position++;
						}

						if (ptr_argument->hasAttribute(Attribute::ReadOnly)) {
							//readonly is ok --> nothing to do
						} else {
							// function may write
							return handle_modification_location(send_call, call);
						}
					}
				} else {
					// no nocapture: function captures the pointer ==> others may access it
					// we cannot do any further analysis
					errs() << "Function "
							<< call->getCalledFunction()->getName()
							<< " captures the send buffer pointer\n No further analysis possible\n";
						return false; // no change made
				}
			}

		} else {
			errs()
					<< "Support for the analysis of this instruction is not implemented yet\n";
			to_handle->print(errs());
			errs() << "\n";
		}

		//TODO handle other instructions such as
		// GEP
		// cast

		// remove erase the analyzed instruction
		to_analyze.erase(
				std::remove(to_analyze.begin(), to_analyze.end(), to_handle),
				to_analyze.end());
		if (to_analyze.size() > 0) {
			to_handle = get_last_instruction(to_analyze);
		} else {
			to_handle = nullptr;
			// nothing more to do
			// no modification detected so far
			return false;
		}

	}

	assert(false && "Analysis should be completed before");
	return false;
}

