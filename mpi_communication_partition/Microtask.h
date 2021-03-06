/*
  Copyright 2013 Michael Blesel
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

#ifndef COMMPART_MICROTASK_H
#define COMMPART_MICROTASK_H

#include <memory>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "llvm/Analysis/LoopInfo.h"

/**
 * Struct with pointers to OpenMP Runtime Library calls for parallel for loops
 **/
struct ParallelForData {
	llvm::CallInst *init;
	llvm::CallInst *fini;
};

/**
 * Struct with pointer to OpenMP Runtime Library calls for reduction pragmas
 **/
struct ReductionData {
	llvm::CallInst *reduce;
	llvm::CallInst *end_reduce;
};

/**
 * This class represent a Openmp Parallel Region (omp.outlined) which is created
 * for each OpenMP parallel section.
 **/
class Microtask {
private:
	// The __kmpc_fork_call instruction in the original code, which calls the OpenMP microtask
	llvm::CallInst *_fork_call;

	// The outlinbed function itself (omp.outlined created by the compiler for OpenMP parallel
	// sections).
	llvm::Function *_function;

	// The Input variables used in this Microtask
	// list of Value in Main Function -> Value in ompoutline function
	std::vector<std::pair<llvm::Value*, llvm::Value*>> _shared_variables;

	// Parallel for inside the microtask
	ParallelForData _parallel_for;

	// Reduction inside the microtask
	ReductionData _reduction;

public:
	/**
	 * Constructor expects a CallInst* to __kmpc_fork_call
	 **/
	Microtask(llvm::CallInst *fork_call);

	~Microtask();

	llvm::CallInst* get_fork_call();

	llvm::Function* get_function();

	ParallelForData* get_parallel_for();
	llvm::Loop* get_chunk_loop();
	llvm::Loop* get_for_loop();

	ReductionData* get_reduction();

	std::vector<std::pair<llvm::Value*, llvm::Value*>>& get_shared_variables();

	// gets the value that corresponds to the given value from main
	llvm::Argument* get_value_in_mikrotask(llvm::Value *val);
	// get the value that corresponds to the given value in microtask
	llvm::Value* get_value_in_main(llvm::Value *val);

	// get end block of loop
	// this means the omp.dispatch.cond.omp.dispatch.end_crit_edge
	// not the omp.dispatch.end block fith the call to for_static_fini function
	//TODO rename function?
	llvm::BasicBlock* find_loop_end_block();

};

#endif
