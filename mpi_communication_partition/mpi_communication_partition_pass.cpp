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

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

#include <assert.h>
//#include <mpi.h>
#include <cstring>
#include <utility>
#include <vector>

#include "additional_assertions.h"
#include "analysis_results.h"
#include "conflict_detection.h"
#include "debug.h"
#include "function_coverage.h"
#include "implementation_specific.h"
#include "mpi_functions.h"
#include "Openmp_region.h"
#include "helper.h"

using namespace llvm;

// declare dso_local i32 @MPI_Recv(i8*, i32, i32, i32, i32, i32,
// %struct.MPI_Status*) #1

RequiredAnalysisResults *analysis_results;

struct mpi_functions *mpi_func;
ImplementationSpecifics *mpi_implementation_specifics;
FunctionMetadata *function_metadata;

namespace {
struct MSGOrderRelaxCheckerPass: public ModulePass {
	static char ID;

	MSGOrderRelaxCheckerPass() :
			ModulePass(ID) {
	}

	// register that we require this analysis

	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<TargetLibraryInfoWrapperPass>();
		AU.addRequiredTransitive<AAResultsWrapperPass>();
		AU.addRequired<LoopInfoWrapperPass>();
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<PostDominatorTreeWrapperPass>();
	}
	/*
	 void getAnalysisUsage(AnalysisUsage &AU) const {
	 AU.addRequiredTransitive<TargetLibraryInfoWrapperPass>();
	 AU.addRequiredTransitive<AAResultsWrapperPass>();
	 AU.addRequiredTransitive<LoopInfoWrapperPass>();
	 AU.addRequiredTransitive<ScalarEvolutionWrapperPass>();
	 }
	 */

	StringRef getPassName() const {
		return "MPI Communication Partition";
	}

	//TODO is there a better option to do it, it seems that this only reverse engenieer the analysis donb by llvm
	// calculates the start value and inserts the calculation into the program
	Value* get_scev_value(const SCEV *scev, Instruction *insert_before) {

		scev->print(errs());
		errs() << "\n";

		if (auto *c = dyn_cast<SCEVUnknown>(scev)) {
			c->getValue()->print(errs());
			errs() << "\n";
			return c->getValue();
		}
		if (auto *c = dyn_cast<SCEVConstant>(scev)) {
			c->getValue()->print(errs());
			errs() << "\n";
			return c->getValue();
		}

		if (auto *c = dyn_cast<SCEVCastExpr>(scev)) {
			IRBuilder<> builder(insert_before);
			errs() << " cast expr\n";
			auto *operand = c->getOperand();

			if (isa<SCEVSignExtendExpr>(c)) {
				return builder.CreateSExt(
						get_scev_value(operand, insert_before), c->getType());
			}
			if (isa<SCEVTruncateExpr>(c)) {
				return builder.CreateTrunc(
						get_scev_value(operand, insert_before), c->getType());
			}
			if (isa<SCEVZeroExtendExpr>(c)) {
				return builder.CreateZExt(
						get_scev_value(operand, insert_before), c->getType());
			}
		}
		if (auto *c = dyn_cast<SCEVCommutativeExpr>(scev)) {
			IRBuilder<> builder(insert_before);
			errs() << " commutative expr\n";
			if (isa<SCEVAddExpr>(c)) {
				errs() << " add expr\n";

				assert(c->getNumOperands() > 1);

				int operand = 1;
				Value *Left_side = get_scev_value(c->getOperand(0),
						insert_before);
				while (operand < c->getNumOperands()) {
					Left_side = builder.CreateAdd(Left_side,
							get_scev_value(c->getOperand(operand),
									insert_before), "", c->hasNoUnsignedWrap(),
							c->hasNoSignedWrap());
					operand++;
				}

				auto *result = Left_side;

				result->print(errs());
				errs() << "\n";
				return result;
			}
			if (isa<SCEVMulExpr>(c)) {
				errs() << "mul expr\n";
				assert(c->getNumOperands() > 1);

				int operand = 1;
				Value *Left_side = get_scev_value(c->getOperand(0),
						insert_before);
				while (operand < c->getNumOperands()) {
					builder.CreateMul(Left_side,
							get_scev_value(c->getOperand(operand),
									insert_before), "", c->hasNoUnsignedWrap(),
							c->hasNoSignedWrap());
					operand++;
				}

				auto *result = Left_side;
				return result;
			}
		}
		if (isa<SCEVMinMaxExpr>(scev)) {
			errs() << " This kind of expression is not supported yet\n";
			assert(false);
		}

		if (auto *c = dyn_cast<SCEVAddRecExpr>(scev)) {
			errs()
					<< " ERROR CALCULATING STARTPOINT: encountered another inductive expression -- this is not supported yet\n";
			assert(false);
		}

		errs() << " ERROR CALCULATING STARTPOINT\n";
		assert(false);

		return nullptr;
	}

	std::pair<Value*, Value*> get_access_pattern(GetElementPtrInst *gep) {

		LoopInfo *linfo = analysis_results->getLoopInfo(
				gep->getParent()->getParent());
		ScalarEvolution *se = analysis_results->getSE(
				gep->getParent()->getParent());

		Loop *loop = linfo->getLoopFor(gep->getParent());

		errs() << "\n";
		errs() << "\n";
		gep->print(errs());
		errs() << "\n";
		auto *sc = se->getSCEV(gep);
		sc->print(errs());
		errs() << "\n";

		errs() << "Loop Computable? "
				<< (se->hasComputableLoopEvolution(sc, loop)) << "\n";

		if (auto *scn = dyn_cast<SCEVAddRecExpr>(sc)) {
			errs() << "Linear? " << scn->isAffine() << "\n";

			scn->getStart()->print(errs());
			errs() << "\n";

			scn->getStepRecurrence(*se)->print(errs());
			errs() << "\n";

			if (scn->isAffine()) {
				//A
				auto step = get_scev_value(scn->getStepRecurrence(*se), gep);
				// x is iteration count
				//B
				auto start = get_scev_value(scn->getStart(), gep);

				return std::make_pair(step, start);
			}

		}

		// not computable
		return std::make_pair(nullptr, nullptr);
	}

	//errs() << "\n";

	// get last instruction  in the sense that the return value post dominates all other instruction in the given list
	// nullptr if not found
	Instruction* get_last_instruction(std::vector<Instruction*> inst_list) {

		if (inst_list.size() == 0) {
			return nullptr;
		}

		Instruction *current_instruction = inst_list[0];
		bool is_viable = true;
		auto *Pdomtree = analysis_results->getPostDomTree(
				current_instruction->getFunction());

		// first entry is current candidate
		for (auto it = inst_list.begin() + 1; it < inst_list.end(); ++it) {
			if (Pdomtree->dominates(*it, current_instruction)) {
				current_instruction = *it;
			} else if (!Pdomtree->dominates(current_instruction, *it)) {
				// no instruction dominates the other
				is_viable = false;
			}
		}

		if (is_viable) {
			return current_instruction;
		} else {
			// need to double-check if this dominates all other instructions
			// Maybe A->C b-> C but the missing order between A and B will set is_viable flag to false
			// therefore we need to recheck
			for (auto *i : inst_list) {
				if (i != current_instruction
						&& !Pdomtree->dominates(current_instruction, i)) {
					// found a non dominate relation: abort
					return nullptr;
				}
			}
			// successfully passed test above
			return current_instruction;

		}

	}

	// return true in modification where done
	bool handle_modification_location(CallInst *send_call,
			Instruction *last_modification) {

		if (last_modification->getNextNode() != send_call) {
			errs()
					<< "Found opportunity to increase the non-blocking window of send call\n";
			send_call->print(errs());
			errs()
					<< "\n Maybe extending the non-blocking window will be part of a future version";

		}

		return false;
	}

	//TODO change discover of buffer ptr
	// buffer ptr may be the higher order ptr (before GEP)
	// send might be done on ptr resulting from GEP

	// returns true if fork call is handled
	// false if thread doesn't write to buffer and nothing need to be done
	bool handle_fork_call(Microtask *parallel_region, CallInst *send_call) {

		auto *buffer_ptr_in_main = send_call->getArgOperand(0);

		auto *buffer_ptr = parallel_region->get_value_in_mikrotask(
				buffer_ptr_in_main);

		if (buffer_ptr->hasNoCaptureAttr()
				&& buffer_ptr->hasAttribute(Attribute::ReadOnly)) {
			// readonly and nocapture: nothing need to be done
			return false;
		}

		if (parallel_region->get_parallel_for()) {
			// we have to build a list of all accesses to the buffer

			auto store_list = get_instruction_in_function<StoreInst>(
					parallel_region->get_function());

			auto call_list = get_instruction_in_function<CallBase>(
					parallel_region->get_function());

			// make shure no called function writes the msg buffer (such calls should be inlined beforehand)

			auto AA = analysis_results->getAAResults(
					parallel_region->get_function());

			for (auto *call : call_list) {

				// check if call is openmp RTL call
				if (!call->getCalledFunction()->getName().startswith(
						"__kmpc_")) {

					//TODO do i need to handle MPi calls seperately?

					for (auto &a : call->args()) {

						if (auto *arg = dyn_cast<Value>(a)) {

							if (arg->getType()->isPointerTy()) {

								if (!AA->isNoAlias(buffer_ptr, arg)) {

									// may alias or must alias
									//TODO Problem ptr is a function arg and may therefore alias with everything in function!

									//TODO check if nocapture and readonly in tgt function

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
				// checked all call instructions

				// need to check all store instructions
				// first we need to check if there is a store PAST the for loop accessing the ptr
				auto *linfo = analysis_results->getLoopInfo(
						parallel_region->get_function());
				// we checed for the presence of ahte openmp loobs before
				assert(!linfo->empty());

				auto *DT = analysis_results->getDomTree(
						parallel_region->get_function());
				auto *PDT = analysis_results->getPostDomTree(
						parallel_region->get_function());

				//TODO implement analysis of multiple for loops??
				auto *loop_exit = parallel_region->get_parallel_for()->fini;


				//TODO It fails in this lambda!
				bool are_all_stores_before_loop_finish = std::all_of(store_list.begin(),
						store_list.end(),
						[AA, PDT, buffer_ptr, loop_exit](llvm::StoreInst *s) {

							errs() << AA;
							errs() << "\n";
							errs() << buffer_ptr;
							buffer_ptr->print(errs());
							//errs() << "\n";
							//errs() << s;
							errs() << "\n";
							errs() << s->getPointerOperand();
							s->getPointerOperand()->print(errs());
							errs() << "\n";
							if (!AA->isNoAlias(buffer_ptr,
									s->getPointerOperand())) {
								// may alias
								return PDT->dominates(loop_exit, s);
							}
							return true;
						});
				errs() << "All before loop exit?" << are_all_stores_before_loop_finish
						<< "\n";
				if (!are_all_stores_before_loop_finish){
					//cannot determine a partitioning if access is outside the loop
					handle_modification_location(send_call,
							parallel_region->get_fork_call());
					return true;
				}

				//TODO do we need to consider stores before the loop?

				// now we need to get min and maximum ax+b for every store in the loop

			}

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

// Pass starts here
	virtual bool runOnModule(Module &M) {

		//Debug(M.dump(););

		M.print(errs(), nullptr);

		mpi_func = get_used_mpi_functions(M);
		if (!is_mpi_used(mpi_func)) {
			// nothing to do for non mpi applications
			delete mpi_func;
			return false;
		}

		bool modification = false;

		analysis_results = new RequiredAnalysisResults(this);

		//function_metadata = new FunctionMetadata(analysis_results->getTLI(), M);

		mpi_implementation_specifics = new ImplementationSpecifics(M);

		// debugging

		Function *F = M.getFunction(".omp_outlined.");

		Function *dbg_func = M.getFunction("debug_function");

		// find MPI Send calls
		for (auto *senders : mpi_func->mpi_send->users()) {

			if (auto *send_call = dyn_cast<CallInst>(senders)) {
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

				for (auto *u : buffer_ptr->users()) {
					if (u != send_call) {

						std::vector<Instruction*> to_analyze;

						if (auto *inst = dyn_cast<Instruction>(u)) {

							if (llvm::isPotentiallyReachable(inst, send_call,
									nullptr, dt, linfo)) {
								// only then it may have an effect
								to_analyze.push_back(inst);
							}
						}

						//TODO need to refactor this!
						Instruction *to_handle = get_last_instruction(
								to_analyze);
						bool handled = false;		// flag to abort
						while (!handled && to_handle != nullptr) {

							to_handle->print(errs());
							errs() << "\n";
							if (auto *store = dyn_cast<StoreInst>(to_handle)) {
								if (buffer_ptr == store->getValueOperand()) {
									// add all uses of the Pointer operand "taint" this pointer and treat it as the msg buffer itself
									// this will lead to false positives
									//but there are not much meaningful uses of this anyway

									for (auto *u : store->getPointerOperand()->users()) {
										if (u != store) {
											if (auto *i = dyn_cast<Instruction>(
													u)) {
												to_analyze.push_back(i);
											}
										}
									}
								} else {
									assert(
											buffer_ptr
													== store->getPointerOperand());
									modification = modification
											| handle_modification_location(
													send_call, to_handle);
									handled = true;
									continue;
								}

							} else if (auto *call = dyn_cast<CallInst>(
									to_handle)) {

								assert(
										call->hasArgument(buffer_ptr)
												&& "You arer calling an MPI sendbuffer?\n I refuse to analyze this dak pointer magic!\n");

								unsigned operand_position = 0;
								while (call->getArgOperand(operand_position)
										!= buffer_ptr) {
									++operand_position;
								}// will terminate, as assert above secured that

								if (operand_position
										>= call->getCalledFunction()->getFunctionType()->getNumParams()) {
									assert(
											call->getCalledFunction()->getFunctionType()->isVarArg());
									operand_position =
											call->getCalledFunction()->getFunctionType()->getNumParams()
													- 1;
									// it is one of the VarArgs
								}

								auto *ptr_argument =
										call->getCalledFunction()->getArg(
												operand_position);

								if (call->getCalledFunction()->getName().equals(
										"__kmpc_fork_call")) {
									// TODO analyze this fork call

									Microtask *parallel_region = new Microtask(
											call);

									handled = handle_fork_call(parallel_region,
											send_call);

								} else {
									if (ptr_argument->hasNoCaptureAttr()) {
										if (call->getCalledFunction()->onlyReadsMemory()) {
											//readonly is ok --> nothing to do
										} else {
											unsigned operand_position = 0;
											while (call->getArgOperand(
													operand_position)
													!= buffer_ptr) {
												operand_position++;
											}

											if (ptr_argument->hasAttribute(
													Attribute::ReadOnly)) {
												//readonly is ok --> nothing to do
											} else {
												// function may write
												modification =
														modification
																| handle_modification_location(
																		send_call,
																		call);
											}
										}
									} else {
										// no nocapture: function captures the pointer ==> others may access it
										// we cannot do any further analysis
										errs() << "Function "
												<< call->getCalledFunction()->getName()
												<< " captures the send buffer pointer\n No further analysis possible\n";
										handled = true;	// fonnd that we cannot do anything
										continue;
									}
								}

							} else {
								errs()
										<< "Support for the analysis of this instruction is not implemented yet\n";
								to_handle->print(errs());
								errs() << "\n";
							}

							// handle other instructions such as
							// GEP
							// cast

							// remove erase the analyzed instruction
							to_analyze.erase(
									std::remove(to_analyze.begin(),
											to_analyze.end(), to_handle),
									to_analyze.end());
							if (to_analyze.size() > 0) {
								to_handle = get_last_instruction(to_analyze);
							} else {
								to_handle = nullptr;
								// nothing more to do
								// no modification detected so far
							}

						}
					}

				}
			}
		}
		// find usage of sending buffer
		// if usage == openmp fork call
		// analyze the parallel region to find if partitioning is possible

		errs() << "Successfully executed the pass\n\n";
		delete mpi_func;
		delete mpi_implementation_specifics;
		delete analysis_results;

		//delete function_metadata;

		return modification;

	}
}
;

// class MSGOrderRelaxCheckerPass
}// namespace

char MSGOrderRelaxCheckerPass::ID = 42;

// Automatically enable the pass.
// http://adriansampson.net/blog/clangpass.html
static void registerExperimentPass(const PassManagerBuilder&,
		legacy::PassManagerBase &PM) {
	PM.add(new MSGOrderRelaxCheckerPass());
}

// static RegisterStandardPasses
//    RegisterMyPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
//                   registerExperimentPass);

static RegisterStandardPasses RegisterMyPass(
		PassManagerBuilder::EP_VectorizerStart, registerExperimentPass);
// before vectorization makes analysis way harder
//This extension point allows adding optimization passes before the vectorizer and other highly target specific optimization passes are executed.

static RegisterStandardPasses RegisterMyPass0(
		PassManagerBuilder::EP_EnabledOnOptLevel0, registerExperimentPass);
