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

#include "analysis_results.h"
#include "debug.h"
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

MpiFunctions *mpi_func;
ImplementationSpecifics *mpi_implementation_specifics;

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

// Pass starts here
	virtual bool runOnModule(Module &M) {

//Debug(M.dump(););

//M.print(errs(), nullptr);
		M.getFunction("main")->dump();

		mpi_func = new MpiFunctions(M);
		if (!mpi_func->is_mpi_used()) {
			// nothing to do for non mpi applications
			delete mpi_func;
			return false;
		}

		bool modification = false;

		analysis_results = new RequiredAnalysisResults(this, &M);

		mpi_implementation_specifics = new ImplementationSpecifics(M);

		for (auto *send_function : mpi_func->get_used_send_functions()) {

			// this iterator might be invalidated if a send call gets removed
			// so we collect all instructions into a vector beforehand
			std::vector<User*>senders;
			std::copy(send_function->user_begin(), send_function->user_end(), std::back_inserter(senders));

			for (auto *s : senders) {

				//TODO do wen need to make shure this send call is actually still valid?
				if (auto *send_call = dyn_cast<CallInst>(s)) {
					modification = modification | handle_send_call(send_call);
				}

			}
		}

		//M.dump();

		// only need to dump the relevant part
/*
		M.getFunction(".omp_outlined.")->dump();
		M.getFunction("main")->dump();
		if (M.getFunction(".omp_outlined._p") != nullptr) {
			M.getFunction(".omp_outlined._p")->dump();
		}*/

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
