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

//TODO clean Includes
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
#include "insert_changes.h"
#include "sending_partitioning.h"

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


	//TODO Legacy code: will remove in one of the next commits
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
	//END Lagacy code





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

// find MPI Send calls
		for (auto *senders : mpi_func->mpi_send->users()) {

			if (auto *send_call = dyn_cast<CallInst>(senders)) {
				modification = modification | handle_send_call(send_call);
			}

		}
		//M.dump();

		// only need to dump the relevant part
		M.getFunction("main")->dump();
		M.getFunction(".omp_outlined.")->dump();
		M.getFunction(".omp_outlined._p")->dump();

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
