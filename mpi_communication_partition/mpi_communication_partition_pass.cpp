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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llvm/IR/Dominators.h"

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

		analysis_results = new RequiredAnalysisResults(this);

		//function_metadata = new FunctionMetadata(analysis_results->getTLI(), M);

		mpi_implementation_specifics = new ImplementationSpecifics(M);

		// debugging

		Function *F = M.getFunction(".omp_outlined.");

		Function *dbg_func = M.getFunction("debug_function");

		LoopInfo *linfo = analysis_results->getLoopInfo(F);
		ScalarEvolution *se = analysis_results->getSE(F);

		for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {

			if (auto *gep = dyn_cast<GetElementPtrInst>(&*I)) {

				Loop *loop = linfo->getLoopFor(gep->getParent());

				errs() << "\n";
				errs() << "\n";
				gep->print(errs(), nullptr);
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

					//A
					auto step = get_scev_value(scn->getStepRecurrence(*se),
							gep);
					// x is iteration count
					//B
					auto start = get_scev_value(scn->getStart(), gep);

				}

			}

			//errs() << "\n";

		}

		errs() << "Successfully executed the pass\n\n";
		delete mpi_func;
		delete mpi_implementation_specifics;
		delete analysis_results;

		//delete function_metadata;

		return false;
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
		PassManagerBuilder::EP_OptimizerLast, registerExperimentPass);

static RegisterStandardPasses RegisterMyPass0(
		PassManagerBuilder::EP_EnabledOnOptLevel0, registerExperimentPass);
