#ifndef CATO_MICROTASK_H
#define CATO_MICROTASK_H

#include <memory>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

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
	llvm::Loop* Microtask::get_for_loop();

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
