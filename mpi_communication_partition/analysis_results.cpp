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

#include "mpi_functions.h"

#include "llvm/Pass.h"

#include "analysis_results.h"

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


using namespace llvm;

RequiredAnalysisResults::RequiredAnalysisResults(Pass *parent_pass, Module* M) {

	assertion_checker_pass = parent_pass;

	assert(
			mpi_func != nullptr
					&& "The search for MPI functions should be made first");

	// just give it any function, the Function is not used at all
	// dont know why the api has changed here...
	TLI =
			&assertion_checker_pass->getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
					*mpi_func->mpi_init);




	current_LI_function = nullptr;
	current_SE_function = nullptr;
	current_AA_function = nullptr;
	current_Dtree_function= nullptr;
	current_PDtree_function= nullptr;
	current_LI = nullptr;
	current_SE = nullptr;
	current_AA = nullptr;
	current_Dtree = nullptr;
	current_PDtree=nullptr;


}

llvm::AAResults* RequiredAnalysisResults::getAAResults(llvm::Function *f) {
	/*if (current_AA_function != f) {
		current_AA_function = f;
		current_AA = &assertion_checker_pass->getAnalysis<AAResultsWrapperPass>(
				*f).getAAResults();
	}*/

	return &assertion_checker_pass->getAnalysis<AAResultsWrapperPass>(
			*f).getAAResults();

	//return current_AA;
}

llvm::LoopInfo* RequiredAnalysisResults::getLoopInfo(llvm::Function *f) {
	if (current_LI_function != f) {
		current_LI_function = f;
		current_LI = &assertion_checker_pass->getAnalysis<LoopInfoWrapperPass>(
				*f).getLoopInfo();
	}

	return current_LI;
}

llvm::DominatorTree* RequiredAnalysisResults::getDomTree(llvm::Function *f) {
	if (current_Dtree_function != f) {
		current_Dtree_function = f;
		current_Dtree = &assertion_checker_pass->getAnalysis<
				DominatorTreeWrapperPass>(*f).getDomTree();
	}
	return current_Dtree;
}

llvm::PostDominatorTree* RequiredAnalysisResults::getPostDomTree(llvm::Function *f) {
	if (current_PDtree_function != f) {
		current_PDtree_function = f;
		current_PDtree = &assertion_checker_pass->getAnalysis<
				PostDominatorTreeWrapperPass>(*f).getPostDomTree();
	}
	return current_PDtree;
}


llvm::ScalarEvolution* RequiredAnalysisResults::getSE(llvm::Function *f) {
	if (current_SE_function != f) {
		current_SE_function = f;
		current_SE = &assertion_checker_pass->getAnalysis<
				ScalarEvolutionWrapperPass>(*f).getSE();
	}

	return current_SE;
}

llvm::TargetLibraryInfo* RequiredAnalysisResults::getTLI() {
	return TLI;
}
