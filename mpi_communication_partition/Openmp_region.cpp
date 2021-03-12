#include "Openmp_region.h"

#include "helper.h"

using namespace llvm;

Microtask::Microtask(CallInst *fork_call) {

	assert(
			fork_call->getCalledFunction()->getName().equals(
					"__kmpc_fork_call"));
	_fork_call = fork_call;
	_parallel_for.init = nullptr;
	_parallel_for.fini = nullptr;
	_reduction.reduce = nullptr;
	_reduction.end_reduce = nullptr;

	// Get the microtask function from the fork calls arguments
	auto *microtask_arg = fork_call->getArgOperand(2);
	_function = dyn_cast<Function>(microtask_arg->stripPointerCasts());

	// Collect the shared variables
	for (auto &argument : _function->args()) {
		// The first shared variable has index 2
		if (argument.getArgNo() > 1) {
			//_shared_variables.push_back(new SharedVariable(&argument, _function));
			Value *in_parallel = &argument;
			// first shared arg
			int start_args_at_fork = 3;
			// first two args of ompoutlined are not interesting
			start_args_at_fork = start_args_at_fork - 2;

			Value *in_serial = _fork_call->getArgOperand(
					start_args_at_fork + argument.getArgNo());
			_shared_variables.push_back(std::make_pair(in_serial, in_parallel));

		}

	}

	// Look for a parallel for inside the microtask;
	// TODO add the other kmpc functions for parallel for pragamas
	auto call_instructions = get_instruction_in_function<CallInst>(_function);
	for (auto &call : call_instructions) {
		if (call->getCalledFunction()->getName().equals(
				"__kmpc_for_static_init_4")) {
			_parallel_for.init = call;
		} else if (call->getCalledFunction()->getName().equals(
				"__kmpc_for_static_fini")) {
			_parallel_for.fini = call;
		}
	}

	// Look for a reduction inside the microtask;
	// TODO add the other kmpc functions for reduction pragmas
	for (auto &call : call_instructions) {
		if (call->getCalledFunction()->getName().equals(
				"__kmpc_reduce_nowait")) {
			_reduction.reduce = call;
		} else if (call->getCalledFunction()->getName().equals(
				"__kmpc_end_reduce_nowait")) {
			_reduction.end_reduce = call;
		}
	}
}

Microtask::~Microtask() {
}

CallInst* Microtask::get_fork_call() {
	return _fork_call;
}

Function* Microtask::get_function() {
	return _function;
}

ParallelForData* Microtask::get_parallel_for() {
	if (_parallel_for.init != nullptr && _parallel_for.fini != nullptr) {
		return &_parallel_for;
	} else {
		return nullptr;
	}
}

ReductionData* Microtask::get_reduction() {
	if (_reduction.reduce != nullptr && _reduction.end_reduce != nullptr) {
		return &_reduction;
	} else {
		return nullptr;
	}
}

std::vector<std::pair<llvm::Value*, llvm::Value*>>& Microtask::get_shared_variables() {
	return _shared_variables;
}

// gets the value that corresponds to the given value from main
llvm::Argument* Microtask::get_value_in_mikrotask(llvm::Value *val) {

	if (_fork_call->hasArgument(val)) {

		auto pos = std::find_if(_shared_variables.begin(),
				_shared_variables.end(),
				[&val](const std::pair<Value*, Value*> &element) {
					return (element.first == val);
				});
		// othewise previous if has captured it
		assert(pos != _shared_variables.end());

		return cast<Argument>((*pos).second);

	} else {
		// it is not passed to this microtask
		return nullptr;
	}
}
// get the value that corresponds to the given value in microtask
llvm::Value* Microtask::get_value_in_main(llvm::Value *val) {
	if (auto *arg = dyn_cast<Argument>(val)) {
		if (arg->getParent() == _function) {

			auto pos = std::find_if(_shared_variables.begin(),
					_shared_variables.end(),
					[&val](const std::pair<Value*, Value*> &element) {
						return (element.second == val);
					});
			assert(pos != _shared_variables.end());
			return (*pos).first;
		}
	}
	return nullptr;

}

BasicBlock* Microtask::find_loop_end_block() {

	if (this->get_parallel_for() == nullptr) {
		return nullptr;
	}

	auto it = find_if_exactly_one(_function->begin(), _function->end(),
			[](BasicBlock &bb) {
				return bb.getName().startswith(
						"omp.dispatch.cond.omp.dispatch.end");
			});

	if (it == _function->end()) {
		errs() << "Error analyzing the loops structure\n";
		return nullptr;
	} else {
		return &*it;
	}

}

Loop* Microtask::get_chunk_loop() {
	auto *LI = analysis_results->getLoopInfo(_function);

	auto *preheader = _parallel_for.init->getParent()->getNextNode();

	return LI->getLoopFor(preheader->getNextNode());

}

//TODO we need to get the real for loop!
Loop* Microtask::get_for_loop() {
	Loop *top_level_loop = get_chunk_loop();
	auto sub_loop_list = top_level_loop->getSubLoops();
	assert(sub_loop_list.size() == 1);
	return top_level_loop->getSubLoops()[0];
}
