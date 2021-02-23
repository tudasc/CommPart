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

// AA implementations
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CFLAndersAliasAnalysis.h"
#include "llvm/Analysis/CFLSteensAliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ObjCARCAliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

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
		//AU.addRequired<AAResultsWrapperPass>();
		AU.addRequired<LoopInfoWrapperPass>();
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<PostDominatorTreeWrapperPass>();

		// various AA implementations
		/*
		 AU.addRequired<ScopedNoAliasAAWrapperPass>();
		 AU.addRequired<TypeBasedAAWrapperPass>();
		 // USING THIS WILL RESULT IN A CRASH there my be a bug
		 AU.addRequired<BasicAAWrapperPass>();
		 AU.addRequired<GlobalsAAWrapperPass>();
		 AU.addRequired<SCEVAAWrapperPass>();

		 // these also result in bug
		 AU.addRequired<CFLAndersAAWrapperPass>();
		 AU.addRequired<CFLSteensAAWrapperPass>();
		 */
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

	//TODO is there a better option to do it, it seems that this only reverse engenieer the analysis done by llvm
	// calculates the start value and inserts the calculation into the program

	//wrapper for debugging:
	Value* get_value_in_serial_part(Value *in_parallel,
			Microtask *parallel_region) {
		auto *result = get_value_in_serial_part_impl(in_parallel,
				parallel_region);

		if (result == nullptr && in_parallel != nullptr) {
			errs() << "Could not find value for \n";
			in_parallel->dump();
		}

		//in_parallel->dump();
		//errs() << " to:\n";
		//result->dump();
		//errs() << "\n";
		return result;
	}
	// if value* is an alloca it will return the value stored there if applicable
	Value* get_value_in_serial_part_impl(Value *in_parallel,
			Microtask *parallel_region) {

		//nullptr --> nullptr
		if (in_parallel == nullptr) {
			return nullptr;
		}

		if (auto *c = dyn_cast<Constant>(in_parallel)) {
			// nothing to do
			return c;
		}

		if (auto *arg = dyn_cast<Argument>(in_parallel)) {
			return parallel_region->get_value_in_main(arg);
		}

		if (auto *load = dyn_cast<LoadInst>(in_parallel)) {

			if (auto *ptr_arg = dyn_cast<Argument>(load->getPointerOperand())) {
				//TODO do we need to handle a load from ptr given to parallel?
				errs()
						<< "Loading from ptr given to parallel is not supported yet\n";
				return nullptr;
			} else if (auto *ptr_arg = dyn_cast<AllocaInst>(
					load->getPointerOperand())) {
				return get_value_in_serial_part(ptr_arg, parallel_region);

			} else {
				errs() << "This is not supported yet\n";
				load->dump();
				load->getPointerOperand()->dump();
				return nullptr;
			}

		}
		if (auto *ptr_arg = dyn_cast<AllocaInst>(in_parallel)) {
			// e.g. the values set by the for_init call

			// as we want the value outside of the omp parallel we just need to get the first value stored
			// if value isnt stored within the first BasicBlock there is no defined value

			Instruction *next_inst = ptr_arg->getNextNode();

			while (next_inst != nullptr) {
				if (auto *s = dyn_cast<StoreInst>(next_inst)) {
					if (s->getPointerOperand() == ptr_arg) {
						// found matching store
						return get_value_in_serial_part(s->getValueOperand(),
								parallel_region);
					}
				}
				next_inst = next_inst->getNextNode();
			}
			//TODO is there any other way a variable is set besides store?

		}

		errs() << "Error finding the vlaue in main:\n";
		in_parallel->dump();
		return nullptr;
	}

// SCEV do not distinguish between ptr and i64 therefore we might need to add casts
// we always cast to i64 as we need this for arithmetic

	Value* getAsInt(Value *v, Instruction *insert_before) {
		return getCastedToCorrectType(v,
				IntegerType::getInt64Ty(v->getContext()), insert_before);
	}

	Value* getCastedToCorrectType(Value *v, Type *t,
			Instruction *insert_before) {
		if (v->getType() == t) {
			return v;
		} else if (auto *c = dyn_cast<ConstantInt>(v)) {
			return ConstantInt::get(t, c->getValue());

		} else {
			if (t->isIntegerTy()) {
				assert(v->getType()->isIntOrPtrTy());
				IRBuilder<> builder(insert_before);
				if (t->isPointerTy()) {
					return builder.CreateIntToPtr(v, t);
				} else if (t->isIntegerTy()) {
					return builder.CreatePtrToInt(v, t);
				} else {
					assert(false && "Should not reach this");
					return nullptr;
				}
			}
		}
	}

// inserts the scev values outside of the parallel part
	Value* get_scev_value_before_parallel_function(const SCEV *scev,
			Instruction *insert_before, Microtask *parallel_region) {

		//scev->print(errs());
		//errs() << "\n";

		if (auto *c = dyn_cast<SCEVUnknown>(scev)) {
			//c->getValue()->print(errs());
			//errs() << "\n";

			return get_value_in_serial_part(c->getValue(), parallel_region);
		}
		if (auto *c = dyn_cast<SCEVConstant>(scev)) {
			//c->getValue()->print(errs());
			//errs() << "\n";
			return get_value_in_serial_part(c->getValue(), parallel_region);
		}

		if (auto *c = dyn_cast<SCEVCastExpr>(scev)) {
			IRBuilder<> builder(insert_before);
			//errs() << " cast expr\n";
			auto *operand = c->getOperand();

			if (isa<SCEVSignExtendExpr>(c)) {
				return builder.CreateSExt(
						get_scev_value_before_parallel_function(operand,
								insert_before, parallel_region), c->getType());
			}
			if (isa<SCEVTruncateExpr>(c)) {
				return builder.CreateTrunc(
						get_scev_value_before_parallel_function(operand,
								insert_before, parallel_region), c->getType());
			}
			if (isa<SCEVZeroExtendExpr>(c)) {
				return builder.CreateZExt(
						get_scev_value_before_parallel_function(operand,
								insert_before, parallel_region), c->getType());
			}
		}
		if (auto *c = dyn_cast<SCEVCommutativeExpr>(scev)) {
			IRBuilder<> builder(insert_before);
			//errs() << " commutative expr\n";
			if (isa<SCEVAddExpr>(c)) {
				//errs() << " add expr\n";
				//c->dump();

				Value *Left_side = getAsInt(
						get_scev_value_before_parallel_function(
								c->getOperand(0), insert_before,
								parallel_region), insert_before);
				int operand = 1;
				while (operand < c->getNumOperands()) {
					Left_side = builder.CreateAdd(Left_side,
							getAsInt(
									get_scev_value_before_parallel_function(
											c->getOperand(operand),
											insert_before, parallel_region),
									insert_before), "", c->hasNoUnsignedWrap(),
							c->hasNoSignedWrap());
					operand++;
				}

				auto *result = Left_side;

				//result->print(errs());
				//errs() << "\n";
				return result;
			}
			if (isa<SCEVMulExpr>(c)) {
				//errs() << "mul expr\n";
				//c->dump();
				assert(c->getNumOperands() > 1);

				int operand = 1;
				Value *Left_side = getAsInt(
						get_scev_value_before_parallel_function(
								c->getOperand(0), insert_before,
								parallel_region), insert_before);
				while (operand < c->getNumOperands()) {
					Left_side= builder.CreateMul(Left_side,
							getAsInt(
									get_scev_value_before_parallel_function(
											c->getOperand(operand),
											insert_before, parallel_region),
									insert_before), "", c->hasNoUnsignedWrap(),
							c->hasNoSignedWrap());
					operand++;
				}

				auto *result = Left_side;
				return result;
			}
		}
		if (auto *c = dyn_cast<SCEVMinMaxExpr>(scev)) {
			errs() << "This kind of expression is not supported yet\n";
			assert(false);
		}

		if (auto *c = dyn_cast<SCEVAddRecExpr>(scev)) {
			//errs() << " ERROR CALCULATING STARTPOINT: encountered another inductive expression -- this is not supported yet\n";
			//errs() << "WARNING: This kind of expression is not fully supported yet\n";
			//errs() << "Wuse tanative anaylisi might lead to errors\n";

			// is actually correct as the outer loop loops through the chnuks
			return get_scev_value_before_parallel_function(c->getStart(),
					insert_before, parallel_region);
			//assert(false);
		}

		errs() << " ERROR CALCULATING STARTPOINT\n";
		assert(false);

		return nullptr;
	}

//TODO is there a better option to do it, it seems that this only reverse engenieer the analysis done by llvm
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

//combine std::find_if and std::none_of to write stl-like find_if_exactly_one
	template<class InputIt, class UnaryPredicate>
	InputIt find_if_exactly_one(InputIt first, InputIt last, UnaryPredicate p) {
		auto it = std::find_if(first, last, p);
		if ((it != last) && std::none_of(std::next(it), last, p))
			return it;
		else
			return last;
	}

	BasicBlock* find_end_block(Microtask *parallel_region) {

		auto it = find_if_exactly_one(parallel_region->get_function()->begin(),
				parallel_region->get_function()->end(),
				[](BasicBlock &bb) {
					return bb.getName().startswith(
							"omp.dispatch.cond.omp.dispatch.end");
				});

		if (it == parallel_region->get_function()->end()) {
			errs() << "Error analyzing the loops structure\n";
			return nullptr;
		} else {
			return &*it;
		}

	}

// only call if the replacement is actually safe
//TODO refactor to be able to make an assertion
	bool insert_partitioning(Microtask *parallel_region, CallInst *send_call,
			const SCEVAddRecExpr *min_adress,
			const SCEVAddRecExpr *max_adress) {

		assert(min_adress->isAffine() && max_adress->isAffine());

//TOOD for a static schedule, there should be a better way of getting the chunk_size!

		auto *LI = analysis_results->getLoopInfo(
				parallel_region->get_function());

		auto *preheader =
				parallel_region->get_parallel_for()->init->getParent()->getNextNode();
		auto *loop = LI->getLoopFor(preheader->getNextNode());

		errs() << "detected possible partitioning for MPI send Operation\n";

// we need to duplicate the original Function to add the MPi Request as an argument

		auto *ftype = parallel_region->get_function()->getFunctionType();

		std::vector<Type*> new_arg_types;

		for (auto *t : ftype->params()) {
			new_arg_types.push_back(t);

		}
// add the ptr to Request
		new_arg_types.push_back(mpi_func->mpix_request_type->getPointerTo());

		auto *new_ftype = FunctionType::get(ftype->getReturnType(),
				new_arg_types, ftype->isVarArg());

//TODO do we need to invalidate the Microtask object? AT THE END OF FUNCTION when all analysis is done
		auto new_name = parallel_region->get_function()->getName() + "_p";

		Function *new_parallel_function = Function::Create(new_ftype,
				parallel_region->get_function()->getLinkage(), new_name,
				parallel_region->get_function()->getParent());

// contains a mapping form all original values to the clone
		ValueToValueMapTy VMap;

// build mapping for all the original arguments
		for (auto arg_orig_iter = parallel_region->get_function()->arg_begin(),
				arg_new_iter = new_parallel_function->arg_begin();
				arg_orig_iter != parallel_region->get_function()->arg_end();
				++arg_orig_iter, ++arg_new_iter) {
			std::pair<Value*, Value*> kv = std::make_pair(arg_orig_iter,
					arg_new_iter);

			VMap.insert(kv);
		}

//only one return in ompoutlined
		SmallVector<ReturnInst*, 1> returns;

// if migrating to newer LLvm false need to become llvm::CloneFunctionChangeType::LocalChangesOnly
		llvm::CloneFunctionInto(new_parallel_function,
				parallel_region->get_function(), VMap, false, returns);
//, NameSuffix, CodeInfo, TypeMapper, Materializer)

// need to add a call to signoff_partitions after a loop iteration has finished
		Instruction *original_finish = parallel_region->get_parallel_for()->fini;
		Instruction *new_finish = cast<Instruction>(VMap[original_finish]);

// all of this analysis happens in old version of the function!
		auto *loop_end_block = find_end_block(parallel_region);
// this block must contain a store to lower bound and upper bound
// ptr comes from init_function
		CallInst *init_call = parallel_region->get_parallel_for()->init;
		auto *ptr_omp_lb = init_call->getArgOperand(4);
		auto *ptr_omp_ub = init_call->getArgOperand(5);

// these need to be instructions in the omp.dispatch.inc block
		Instruction *omp_lb = nullptr;
		Instruction *omp_ub = nullptr;

		for (auto &inst : *loop_end_block) {
			if (auto *store = dyn_cast<StoreInst>(&inst)) {
				if (store->getPointerOperand() == ptr_omp_lb) {
					assert(
							omp_lb == nullptr
									&& "Only one store to .omp.lb is allowed in this block");
					omp_lb = cast<Instruction>(store->getValueOperand());
				}
				if (store->getPointerOperand() == ptr_omp_ub) {
					assert(
							omp_ub == nullptr
									&& "Only one store to .omp.ub is allowed in this block");
					omp_ub = cast<Instruction>(store->getValueOperand());
				}
			}
		}

		assert(omp_lb != nullptr && omp_ub != nullptr);
		assert(omp_lb->getParent() == omp_ub->getParent());
		assert(loop_end_block->getPrevNode() == omp_lb->getParent());

// now transition to the copy of parallel func including the request parameter:
		omp_lb = cast<Instruction>(VMap[omp_lb]);
		omp_ub = cast<Instruction>(VMap[omp_ub]);

// assertions must still hold
		assert(omp_lb != nullptr && omp_ub != nullptr);
		assert(omp_lb->getParent() == omp_ub->getParent());

//assert(loop_end_block->getPrevNode() == omp_lb->getParent());
// if we want to test this assertion we need to first get the loop_end_block in the copy

// insert before final instruction of this block
		Instruction *insert_point_in_copy =
				omp_lb->getParent()->getTerminator();
		IRBuilder<> builder_in_copy(insert_point_in_copy);
//TODO is there a different insert point for dynamic scheduled loops?

// it is the last argument
		Value *request = new_parallel_function->getArg(
				new_parallel_function->getFunctionType()->getNumParams() - 1);

// maybe we need a sign_extend form i32 to i64

		Type *loop_bound_type =
				mpi_func->signoff_partitions_after_loop_iter->getFunctionType()->getParamType(
						1);
		assert(
				loop_bound_type
						== mpi_func->signoff_partitions_after_loop_iter->getFunctionType()->getParamType(
								2));

		if (omp_lb->getType() != loop_bound_type) {
			omp_lb = cast<Instruction>(
					builder_in_copy.CreateSExt(omp_lb, loop_bound_type));
		}

		if (omp_ub->getType() != loop_bound_type) {
			omp_ub = cast<Instruction>(
					builder_in_copy.CreateSExt(omp_ub, loop_bound_type));
		}

		builder_in_copy.CreateCall(mpi_func->signoff_partitions_after_loop_iter,
				{ request, omp_lb, omp_ub });

// DONE with modifying the parallel region
// the fork_call will be modified later

//		mpi_func->signoff_partitions_after_loop_iter;

//call void @__kmpc_for_static_init_4(%struct.ident_t* nonnull @3, i32 %4, i32 33, i32* nonnull %.omp.is_last, i32* nonnull %.omp.lb, i32* nonnull %.omp.ub, i32* nonnull %.omp.stride, i32 1, i32 1000) #8

//int partition_sending_op(void *buf, MPI_Count count, MPI_Datatype datatype,
//int dest, int tag, MPI_Comm comm, MPIX_Request *request,
// loop info
// access= pattern ax+b
//long A_min, long B_min, long A_max, long B_max, long chunk_size,
//long loop_min, long loop_max)
// collect all arguments for the partitioned call

		auto *insert_point = parallel_region->get_fork_call();
		IRBuilder<> builder(insert_point);

// args form original send
		Value *buf = send_call->getArgOperand(0);
		Value *count = send_call->getArgOperand(1);
		Value *datatype = send_call->getArgOperand(2);
		Value *dest = send_call->getArgOperand(3);
		Value *tag = send_call->getArgOperand(4);
		Value *comm = send_call->getArgOperand(5);

//TODO do we need to check if all of those values are accessible from this function?

// MPI_Request
		Value *request_ptr = builder.CreateAlloca(mpi_func->mpix_request_type,
				nullptr, "mpix_request");

		auto *SE = analysis_results->getSE(parallel_region->get_function());
		//TODO insert it at top of function

// arguments for partitioning

		Value *A_min = get_scev_value_before_parallel_function(
				min_adress->getStepRecurrence(*SE), insert_point,
				parallel_region);

		Value *B_min = get_scev_value_before_parallel_function(
				min_adress->getStart(), insert_point, parallel_region);

//{{((4 * (sext i32 %7 to i64))<nsw> + %buffer)<nsw>,+,(4 * (sext i32 %8 to i64))<nsw>}<%omp.inner.for.cond.preheader>,+,4}<%omp.inner.for.body>
//{{((4 * (sext i32 %7 to i64))<nsw> + %buffer)<nsw>,+,(4 * (sext i32 %8 to i64))<nsw>}<%omp.inner.for.cond.preheader>,+,4}<%omp.inner.for.body>
		// min should be equal to max im my example

		Value *A_max = get_scev_value_before_parallel_function(
				max_adress->getStepRecurrence(*SE), insert_point,
				parallel_region);

		Value *B_max = get_scev_value_before_parallel_function(
				max_adress->getStart(), insert_point, parallel_region);

// 8th parameter of static_for_init
		Value *chunk_size = get_value_in_serial_part(
				parallel_region->get_parallel_for()->init->getArgOperand(8),
				parallel_region);

// 4th parameter of static_for_init
		Value *loop_min = get_value_in_serial_part(
				parallel_region->get_parallel_for()->init->getArgOperand(4),
				parallel_region);

// 5th parameter of static_for_init
		Value *loop_max = get_value_in_serial_part(
				parallel_region->get_parallel_for()->init->getArgOperand(5),
				parallel_region);

		std::vector<Value*> argument_list_with_wrong_types { buf, count,
				datatype, dest, tag, comm, request_ptr, A_min, B_min, A_max,
				B_max, chunk_size, loop_min, loop_max };

// add a sign extension for all args if needed
		std::vector<Value*> argument_list;
		argument_list.reserve(argument_list_with_wrong_types.size());

		std::transform(argument_list_with_wrong_types.begin(),
				argument_list_with_wrong_types.end(),
				mpi_func->partition_sending_op->getFunctionType()->param_begin(),
				std::back_inserter(argument_list),
				[insert_point](Value *v, Type *desired_t) {
					if (v->getType() == desired_t) {
						return v;
					} else {
						// i think passing the builder itself into this lambda might not be a good style
						// so i capture the insertion point instead
						// as it is a insert before semantic
						IRBuilder<> builder(insert_point);
						return builder.CreateSExt(v, desired_t);
					}
				});

		//errs() << "Collected all Values: insert partitioning call\n";
		builder.SetInsertPoint(insert_point);

		builder.CreateCall(mpi_func->partition_sending_op, argument_list,
				"partitions");

		// start the communication
		builder.CreateCall(mpi_func->mpix_Start, { request_ptr });

		// fork_call
		//call void (%struct.ident_t*, i32, void (i32*, i32*, ...)*, ...) @__kmpc_fork_call(%struct.ident_t* nonnull @2, i32 2, void (i32*, i32*, ...)* bitcast (void (i32*, i32*, i32*, i32*)* @.omp_outlined. to void (i32*, i32*, ...)*), i8* %call3, i32* nonnull %rank)
		// change the call to the new ompoutlined

		auto *original_fork_call = parallel_region->get_fork_call();
		auto original_arg_it = original_fork_call->arg_begin();

		Value *loc = *original_arg_it;
		// no need to change it
		original_arg_it = std::next(original_arg_it);// ++arg_it but std::next is the portable version for all iterators

		Value *original_argc = *original_arg_it;
		// we need to increment it as we add the MPI Request
		assert(isa<ConstantInt>(original_argc));
		auto *original_argc_constant = cast<ConstantInt>(original_argc);
		long outlined_arg_count = original_argc_constant->getSExtValue();
		Value *new_argc = ConstantInt::get(original_argc_constant->getType(),
				outlined_arg_count + 1);
		original_arg_it = std::next(original_arg_it);

		// the microtask
		Value *original_microtask = *original_arg_it;
		Value *new_microtask_arg = builder.CreateBitOrPointerCast(
				new_parallel_function, original_microtask->getType());

		original_arg_it = std::next(original_arg_it);

		std::vector<Value*> new_args { loc, new_argc, new_microtask_arg };
		new_args.reserve(3 + outlined_arg_count + 1);
		// all the original arguments
		std::copy(original_arg_it, original_fork_call->arg_end(),
				std::back_inserter(new_args));
		// and the MPI request
		new_args.push_back(request_ptr);

		errs() << "insert new fork call\n";
		auto *new_call = builder.CreateCall(
				original_fork_call->getCalledFunction(), new_args);

		Function *old_ompoutlined = original_fork_call->getCalledFunction();
		// remove old call
		original_fork_call->replaceAllUsesWith(new_call);// unnecessary as it is c void return anyway
		original_fork_call->eraseFromParent();
		// remove old function if no longer needed
		if (old_ompoutlined->user_empty()) {
			old_ompoutlined->eraseFromParent();
			errs() << "Removed the old ompoutlined\n";
		} else {
			//TODO why is ther a user left?
			for (auto *u : old_ompoutlined->users())
				u->dump();
		}

		// now we need to replace the send call with the wait
		builder.SetInsertPoint(send_call);

		//TODO set status ignore instead?

		Type *MPI_status_ptr_type =
				mpi_func->mpix_Wait->getFunctionType()->getParamType(1);

		Value *status_ptr = builder.CreateAlloca(
				MPI_status_ptr_type->getPointerElementType(), 0, "mpi_status");

		Value *new_send_call = builder.CreateCall(mpi_func->mpix_Wait, {
				request_ptr, status_ptr });
		// and finally remove the old send call
		send_call->replaceAllUsesWith(new_send_call);
		send_call->eraseFromParent();

		return true;
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

			auto *AA = &getAnalysis<AAResultsWrapperPass>(
					*parallel_region->get_function()).getAAResults();

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

			AA = analysis_results->getAAResults(
					parallel_region->get_function());

			// filter out all stores that can not alias
			store_list.erase(
					std::remove_if(store_list.begin(), store_list.end(),
							[AA, buffer_ptr](llvm::StoreInst *s) {
								return AA->isNoAlias(buffer_ptr,
										s->getPointerOperand());
							}),store_list.end());

			// check if all remaining are within (or before the loop)
			bool are_all_stores_before_loop_finish = std::all_of(
					store_list.begin(), store_list.end(),
					[PDT, loop_exit](llvm::StoreInst *s) {

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

				if (SE->isKnownPredicate(CmpInst::Predicate::ICMP_SLE,
						candidate, min)) {
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

// Pass starts here
	virtual bool runOnModule(Module &M) {

//Debug(M.dump(););

		//M.print(errs(), nullptr);

		mpi_func = get_used_mpi_functions(M);
		if (!is_mpi_used(mpi_func)) {
			// nothing to do for non mpi applications
			delete mpi_func;
			return false;
		}

		bool modification = false;

		analysis_results = new RequiredAnalysisResults(this, &M);

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

				// gather all uses of this buffer
				std::vector<Instruction*> to_analyze;
				for (auto *u : buffer_ptr->users()) {
					if (u != send_call) {

						if (auto *inst = dyn_cast<Instruction>(u)) {

							if (llvm::isPotentiallyReachable(inst, send_call,
									nullptr, dt, linfo)) {
								// only then it may have an effect
								to_analyze.push_back(inst);
							}
						}
					}
				}

				//TODO need to refactor this!
				Instruction *to_handle = get_last_instruction(to_analyze);
				bool handled = false;				// flag to abort
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
									if (auto *i = dyn_cast<Instruction>(u)) {
										to_analyze.push_back(i);
									}
								}
							}
						} else {
							assert(buffer_ptr == store->getPointerOperand());
							modification = modification
									| handle_modification_location(send_call,
											to_handle);
							handled = true;
							continue;
						}

					} else if (auto *call = dyn_cast<CallInst>(to_handle)) {

						assert(
								call->hasArgument(buffer_ptr)
										&& "You arer calling an MPI sendbuffer?\n I refuse to analyze this dak pointer magic!\n");

						unsigned operand_position = 0;
						while (call->getArgOperand(operand_position)
								!= buffer_ptr) {
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

							handled = handle_fork_call(parallel_region,
									send_call);

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

									if (ptr_argument->hasAttribute(
											Attribute::ReadOnly)) {
										//readonly is ok --> nothing to do
									} else {
										// function may write
										modification = modification
												| handle_modification_location(
														send_call, call);
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

					//TODO handle other instructions such as
					// GEP
					// cast

					// remove erase the analyzed instruction
					to_analyze.erase(
							std::remove(to_analyze.begin(), to_analyze.end(),
									to_handle), to_analyze.end());
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
// find usage of sending buffer
// if usage == openmp fork call
// analyze the parallel region to find if partitioning is possible

		//M.dump();

		// only need to dump the relevant part
		M.getFunction("main")->dump();
		M.getFunction(".omp_outlined.")->dump();
		//M.getFunction(".omp_outlined._p")->dump();

		bool broken_dbg_info;
		bool module_errors = verifyModule(M, &errs(), &broken_dbg_info);

		if (!module_errors) {
			errs() << "Successfully executed the pass\n\n";

		} else {
			errs() << "Module Verification ERROR\n";
		}

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
//
static RegisterStandardPasses RegisterMyPass(
		PassManagerBuilder::EP_VectorizerStart, registerExperimentPass);
// before vectorization makes analysis way harder
//This extension point allows adding optimization passes before the vectorizer and other highly target specific optimization passes are executed.

static RegisterStandardPasses RegisterMyPass0(
		PassManagerBuilder::EP_EnabledOnOptLevel0, registerExperimentPass);
