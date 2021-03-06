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

#include "insert_changes.h"
#include "debug.h"
#include "helper.h"
#include "analysis_results.h"
#include "mpi_functions.h"
#include "mpi_analysis.h"
#include "sending_partitioning.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "Microtask.h"

using namespace llvm;

//TODO evaluate this implementation
// is his enough to move code out of loop if invariant?
Instruction* move_to_maximum_upwards_point(Instruction *inst,
		std::vector<Instruction*> ignore) {

	assert(inst != nullptr);

	//TODO how to move out ptr loop in this design?

	// each param seperately and then ask for if loop of patition call is still same

	if (auto *call = dyn_cast<CallInst>(inst)) {
		Debug(
				errs() << "will not move call instruction\n";
				call->dump();
		)
		return inst;
	} else if (auto *load = dyn_cast<LoadInst>(inst)) {

		auto *move_after = get_latest_modification_of_pointer(
				load->getPointerOperand(), load, ignore);
		inst->moveAfter(move_after);
		return inst;

		// why have a cast or an Sext not inst->isUnaryOp()==true??
	} else if (inst->isUnaryOp() || isa<UnaryInstruction>(inst)) {

		auto *operand = inst->getOperand(0);
		if (auto *operand_inst = dyn_cast<Instruction>(operand)) {
			auto *move_after = move_to_maximum_upwards_point(operand_inst,
					ignore);
			inst->moveAfter(move_after);
			return inst;
		} else {
			// constant or parameter:
			//can move at the beginning of func
			auto *move_after =
					inst->getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
			inst->moveAfter(move_after);
			return inst;
		}

	} else if (inst->isBinaryOp()) {

		auto *move_after =
				inst->getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
		auto *operand0 = inst->getOperand(0);
		if (auto *operand_inst = dyn_cast<Instruction>(operand0)) {
			move_after = move_to_maximum_upwards_point(operand_inst, ignore);
		}
		auto *operand1 = inst->getOperand(1);
		if (auto *operand_inst = dyn_cast<Instruction>(operand1)) {
			auto *other_operand = move_to_maximum_upwards_point(operand_inst,
					ignore);

			if (is_instruction_before(move_after, other_operand)) {
				move_after = other_operand;
			}
		}
		inst->moveAfter(move_after);
		return inst;
	} else if (auto *phi = dyn_cast<PHINode>(inst)) {
		// cannot move up further
		// we need to insert after ALL the PHIs
		// so return the last PHI of block
		return inst->getParent()->getFirstNonPHI()->getPrevNode();

	} else {
		errs()
				<< "no analysis for moving (will not move further to extend nonblocking window):\n";
		inst->dump();
		return inst;
	}

	assert(false && "SHOULD NEVER REACH THIS");
	return nullptr;

}

// will move init call out of the loop if possible
// and "up" as much as possible
// will move free call accordingly (out of the loop if necessary)
void move_init_and_free_call(CallInst *init_call, CallInst *free_call,
		CallInst *fork_call) {

//TODO move "up" all args as much as possible

// will not cross function boundary
	Instruction *maximum_upwards_point =
			init_call->getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();

// no need to ignore free call, it will not interfere with our analysis
	std::vector<Instruction*> ignore = { fork_call, init_call };

// check for the maximum upwards point for each of the arguments
	for (unsigned int i = 0; i < init_call->getNumArgOperands(); ++i) {

		// Request was inserted by us, no need to trace it
		if (i != 6) {
			// if no instruction: e.g. constatnt or func arg: no need to handle it, it can always be moved up
			if (auto *arg = dyn_cast<Instruction>(
					init_call->getArgOperand(i))) {

				auto *moved_to = move_to_maximum_upwards_point(arg, ignore);

				if (is_instruction_before(maximum_upwards_point, moved_to)) {
					maximum_upwards_point = moved_to;
				}

			}
		}
	}

	// not before MPI) init though
	for (auto *u : mpi_func->mpi_init->users()) {
		if (auto *c = dyn_cast<CallInst>(u)) {
			if (c->getFunction() == init_call->getFunction()) {
				if (is_instruction_before(maximum_upwards_point, c)) {
					maximum_upwards_point = c;
				}
			}
		}
	}

	auto *linfo = analysis_results->getLoopInfo(init_call->getFunction());
	if (auto *loop = linfo->getLoopFor(init_call->getParent())) {

		// -1 as we move call to loop exit block of inner loop
		int moved_up_loop_depth = linfo->getLoopDepth(init_call->getParent())
				- linfo->getLoopDepth(maximum_upwards_point->getParent() - 1);

		Debug(errs() << "moved_up_loop_depth: " << moved_up_loop_depth <<"\n";)
		if (moved_up_loop_depth > 0) {
			auto *loop_to_move = loop;
			for (int i = 0; i < moved_up_loop_depth - 1; ++i) {
				loop_to_move = loop->getParentLoop();
			}
			// move call to loop exit block

			assert(loop_to_move != nullptr);
			free_call->moveAfter(
					loop_to_move->getExitBlock()->getFirstNonPHIOrDbgOrLifetime());

		}
	}
	init_call->moveAfter(maximum_upwards_point);

//vorgehen:
//f??r jedes arg: den maximum upwards point bestimmen und die instr entsprechend moven --> keine ??nderung an semantik

// eventuiell outside of loopp moven if all args are loop invariant
//TODO bestimme, ob load loop invariant ist

// was ich besser als clang kan: ??ber fork call moven
// ??ber mpi calls moven

}

//TODO implement
// TODO add call to this in insert_partitioning
// move wait call as much "down" as possible ==> greater overlap
void move_wait_call() {

	errs() << "Extending the non-blocking window is not implemented Yet\n";
}

// move wait call as much "up" as possible ==> greater overlap
void move_start_call() {
	errs() << "Extending the non-blocking window is not implemented Yet\n";
}

//TODO is there a better option to do it, it seems that this only reverse engenieer the analysis done by llvm
// calculates the start value and inserts the calculation into the program

Value* get_value_in_serial_part_impl(Value *in_parallel,
		Microtask *parallel_region);

//wrapper for debugging:
Value* get_value_in_serial_part(Value *in_parallel,
		Microtask *parallel_region) {
	auto *result = get_value_in_serial_part_impl(in_parallel, parallel_region);

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

// we will add the corresponding add or sub after the last instruction in serial
// Openmp will sub 1 from loop bound as the openmp function expect loop bound as inclusive
Value* get_instruction_in_serial_part(Instruction *in_parallel,
		Microtask *parallel_region) {

	//TODO is the is_allowed Part Debug code?
	bool is_allowed = false;

	// unary
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::SExt);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::ZExt);

	// cast
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::Trunc);

	// binary
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::Add);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::Sub);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::Mul);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::AShr);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::LShr);
	is_allowed = is_allowed | (in_parallel->getOpcode() == Instruction::Shl);

//TODO add more allowed instructions if needed
// only control-flow instructions such as branches are forbidden

	if (!is_allowed) {
		errs() << "Currently not supported:\n";
		in_parallel->dump();
		return nullptr;
	}

// find all operands in serial
	std::vector<Value*> operands_in_serial;
	operands_in_serial.reserve(in_parallel->getNumOperands());

	std::transform(in_parallel->op_begin(), in_parallel->op_end(),
			std::back_inserter(operands_in_serial),
			[parallel_region](Value *v) {
				return get_value_in_serial_part(v, parallel_region);
			});

	assert(operands_in_serial.size() == in_parallel->getNumOperands());

// found all operands in serial?
	if (!std::all_of(operands_in_serial.begin(), operands_in_serial.end(),
			[](auto *v) {
				return (v != nullptr);
			})) {
		errs() << "not found all values in serial";
		return nullptr;
	}

// find insertion point
	std::vector<Instruction*> operands_in_serial_as_instructions;
	operands_in_serial_as_instructions.reserve(operands_in_serial.size());

	std::transform(operands_in_serial.begin(), operands_in_serial.end(),
			std::back_inserter(operands_in_serial_as_instructions),
			[](Value *v) {
				return dyn_cast<Instruction>(v);
			});

// remove all nullptrs
	remove_eraze_nullptr(operands_in_serial_as_instructions);

	Instruction *insert_point = get_last_instruction(
			operands_in_serial_as_instructions);

// after the last operand

	insert_point = insert_point->getNextNode();
	if (insert_point == nullptr) {
		// if unknown: use the fork call
		insert_point = parallel_region->get_fork_call();
	}

	IRBuilder<> builder(insert_point);

	if (Instruction::isCast(in_parallel->getOpcode())) {
		// cast is no unary instruction
		return builder.CreateCast(cast<CastInst>(in_parallel)->getOpcode(),
				operands_in_serial[0], in_parallel->getType(),
				in_parallel->getName());
	} else {

		return builder.CreateNAryOp(in_parallel->getOpcode(),
				operands_in_serial, in_parallel->getName());
	}

	return nullptr;
}

Value* get_value_stored_before_parallel_region(Value *ptr_in_serial,
		Microtask *parallel_region) {
//errs() << "get value of Firstprivate var\n";

	assert(ptr_in_serial != nullptr);

	auto *PDtree = analysis_results->getPostDomTree(
			parallel_region->get_fork_call()->getFunction());
	auto *Dtree = analysis_results->getDomTree(
			parallel_region->get_fork_call()->getFunction());

// we only need to take care about stored before the fork call
	std::vector<Instruction*> store_list;
	for (auto *u : ptr_in_serial->users()) {
		if (auto *i = dyn_cast<Instruction>(u)) {
			// if not proven after fork
			if (!Dtree->dominates(parallel_region->get_fork_call(), i)
					&& !PDtree->dominates(parallel_region->get_fork_call(),
							i)) {
				store_list.push_back(i);
			}
		}
	}

	if (store_list.size() != 0) {
		auto *last_i = get_last_instruction(store_list);

		if (auto *last_store = dyn_cast_or_null<StoreInst>(last_i)) {
			if (last_store->getPointerOperand() == ptr_in_serial) {

				//errs() << "Fund value of var\n";
				//last_store->dump();

				return last_store->getValueOperand();

			} else {
				assert(
						false
								&& "detected something that is currently not supported\n");
				return nullptr;
				//TODO handle this case?
			}

		} else {
			errs() << "Last operation to shared var is not a store:";
			last_i->dump();
			return nullptr;
		}
	} else {
		// not found uses of this pointer?
		assert(false && "Error: not find uses of ptr");
	}
// not found
	return nullptr;
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
			//handle a load from ptr given to parallel
			// only do it if it is firstprivate (e.g. readonly)
			if (ptr_arg->hasAttribute(Attribute::ReadOnly)) {
				// we need to find the value stored to this pointer in serial part

				Value *ptr_in_serial = parallel_region->get_value_in_main(
						ptr_arg);
				return get_value_stored_before_parallel_region(ptr_in_serial,
						parallel_region);

			} else {
				errs()
						<< "Fund that the message partitioning depend on a shared variablle.\n"
						<< " Try to use firstprivate clause wherever possible to enable message partitioning\n";
				return nullptr;
			}

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

		Instruction *next_inst = ptr_arg->getNextNode();

		while (next_inst != nullptr) {
			if (auto *s = dyn_cast<StoreInst>(next_inst)) {
				if (s->getPointerOperand() == ptr_arg) {
					// found matching store
					return get_value_in_serial_part(s->getValueOperand(),
							parallel_region);
				}
			}
			if (auto *br = dyn_cast<BranchInst>(next_inst)) {
				//somtimes openmp will skip the loop directly here
				// check if false block only leads to return
				if (isa<ReturnInst>(br->getSuccessor(1)->getFirstNonPHI())) {
					next_inst = br->getSuccessor(0)->getFirstNonPHI();
				}

			} else {
				next_inst = next_inst->getNextNode();
			}
		}
		//TODO is there any other way a variable is set besides store?

	}
// also allowed: add or sub with values known in serial
	if (auto *inst = dyn_cast<Instruction>(in_parallel)) {
		return get_instruction_in_serial_part(inst, parallel_region);
	}

	errs() << "Error finding the vlaue in main:\n";
	in_parallel->dump();
	return nullptr;
}

Value* getCastedToCorrectType(Value *v, Type *t, Instruction *insert_before) {
	if (v->getType() == t) {
		return v;
	} else if (auto *c = dyn_cast<ConstantInt>(v)) {
		return ConstantInt::get(t, c->getSExtValue());

	} else {
		if (t->isIntegerTy()) {
			assert(v->getType()->isIntOrPtrTy());
			IRBuilder<> builder(insert_before);
			if (v->getType()->isPointerTy()) {
				return builder.CreatePtrToInt(v, t);
			} else {
				assert(v->getType()->isIntegerTy());
				return builder.CreateSExtOrTrunc(v, t);
			}

		} else {
			// other than integer
			assert(
					false
							&& "Should not reach this, this is only supposed to cast to int");
			return nullptr;
		}
	}
}

// SCEV do not distinguish between ptr and i64 therefore we might need to add casts
// we always cast to i64 as we need this for arithmetic

Value* getAsInt(Value *v, Instruction *insert_before) {
	return getCastedToCorrectType(v, IntegerType::getInt64Ty(v->getContext()),
			insert_before);
}

//TODO refactoring and move this part to a different file?
Value* get_scev_value_before_parallel_function(const SCEV *scev,
		Instruction *insert_before, Microtask *parallel_region);

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
					get_scev_value_before_parallel_function(c->getOperand(0),
							insert_before, parallel_region), insert_before);
			unsigned int operand = 1;
			while (operand < c->getNumOperands()) {
				Left_side = builder.CreateAdd(Left_side,
						getAsInt(
								get_scev_value_before_parallel_function(
										c->getOperand(operand), insert_before,
										parallel_region), insert_before), "",
						c->hasNoUnsignedWrap(), c->hasNoSignedWrap());
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

			unsigned int operand = 1;
			Value *Left_side = getAsInt(
					get_scev_value_before_parallel_function(c->getOperand(0),
							insert_before, parallel_region							 ),
					insert_before);
			while (operand < c->getNumOperands()) {
				Left_side = builder.CreateMul(Left_side,
						getAsInt(
								get_scev_value_before_parallel_function(
										c->getOperand(operand), insert_before,
										parallel_region										), insert_before), "",
						c->hasNoUnsignedWrap(), c->hasNoSignedWrap());
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
		// use the start value
		return get_scev_value_before_parallel_function(c->getStart(), insert_before, parallel_region);
	}

	errs() << " ERROR CALCULATING STARTPOINT\n";
	assert(false);

	return nullptr;
}

// return true if modification where done
bool handle_modification_location(CallInst *send_call,
		Instruction *last_modification) {

	if (last_modification->getNextNode() != send_call) {
		errs()
				<< "Found opportunity to increase the non-blocking window of send call\n";
		send_call->print(errs());
		errs() << "\nlastly modified at\n";
		last_modification->print(errs());
		errs()
				<< "\n Maybe extending the non-blocking window will be part of a future version";

	}

	return false;
}

// duplicates the parallel_region adding the mpi_request parameter
// will maorfy vmpap to contain the mapping old vars -> new vars
// expect empty vmap
Function* duplicate_parallel_function_with_added_request(
		Microtask *parallel_region, ValueToValueMapTy &VMap) {

	auto *ftype = parallel_region->get_function()->getFunctionType();

	std::vector<Type*> new_arg_types;

	for (auto *t : ftype->params()) {
		new_arg_types.push_back(t);

	}
// add the ptr to Request
	new_arg_types.push_back(mpi_func->mpix_request_type->getPointerTo());

	auto *new_ftype = FunctionType::get(ftype->getReturnType(), new_arg_types,
			ftype->isVarArg());

//TODO do we need to invalidate the Microtask object? AT THE END OF FUNCTION when all analysis is done
	auto new_name = parallel_region->get_function()->getName() + "_p";

	Function *new_parallel_function = Function::Create(new_ftype,
			parallel_region->get_function()->getLinkage(), new_name,
			parallel_region->get_function()->getParent());

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

	return new_parallel_function;
}

void add_partition_signoff_call(ValueToValueMapTy &VMap,
		Microtask *parallel_region, Function *new_parallel_function) {
// need to add a call to signoff_partitions after a loop iteration has finished

// all of this analysis happens in old version of the function!
	auto *loop_end_block = parallel_region->find_loop_end_block();
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

// we want the old lower bound of the passed iteration, not the new one for the next iter
	Instruction *add_lb = cast<Instruction>(omp_lb);
	assert(add_lb->getOpcode() == Instruction::Add);
	omp_lb = cast<Instruction>(add_lb->getOperand(1));
//TODO assert that the other operand of add is the value of %omp_stride ?

// same for the upper bound but need to handle the select instr first
	auto *select_inst = cast<SelectInst>(omp_ub);
	omp_ub = cast<Instruction>(select_inst->getTrueValue());
	Instruction *add_ub = cast<Instruction>(omp_ub);
	assert(add_ub->getOpcode() == Instruction::Add);
	omp_ub = cast<Instruction>(add_ub->getOperand(1));

//TODO assert that the other operand of add is the value of %omp_stride ?
	assert(add_ub->getOperand(0) == add_lb->getOperand(0)); // at least the stride must be equal

// insert point is at the end of a loop chunk
	Instruction *original_insert_point = add_lb->getParent()->getTerminator();
//if we add att the location of omp_lb or omp_ub this would be at the beginning

// now transition to the copy of parallel func including the request parameter:
	omp_lb = cast<Instruction>(VMap[omp_lb]);
	omp_ub = cast<Instruction>(VMap[omp_ub]);
//TODO these are the wrong values!!!
// assertions must still hold
	assert(omp_lb != nullptr && omp_ub != nullptr);
	assert(omp_lb->getParent() == omp_ub->getParent());
//assert(loop_end_block->getPrevNode() == omp_lb->getParent());
// if we want to test this assertion we need to first get the loop_end_block in the copy
// insert before final instruction of this block
	Instruction *insert_point_in_copy = cast<Instruction>(
			VMap[original_insert_point]);
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
	builder_in_copy.CreateCall(mpi_func->signoff_partitions_after_loop_iter, {
			request, omp_lb, omp_ub });
}

CallInst* add_partition_init_call(Instruction *insert_point, Value *request_ptr,
		Microtask *parallel_region, CallInst *send_call,
		const SCEVAddRecExpr *min_adress, const SCEVAddRecExpr *max_adress) {

//call void @__kmpc_for_static_init_4(%struct.ident_t* nonnull @3, i32 %4, i32 33, i32* nonnull %.omp.is_last, i32* nonnull %.omp.lb, i32* nonnull %.omp.ub, i32* nonnull %.omp.stride, i32 1, i32 1000) #8

//int partition_sending_op(void *buf, MPI_Count count, MPI_Datatype datatype,
//int dest, int tag, MPI_Comm comm, MPIX_Request *request,
// loop info
// access= pattern ax+b
//long A_min, long B_min, long A_max, long B_max, long chunk_size,
//long loop_min, long loop_max)
// collect all arguments for the partitioned call

// args from original send
	Value *buf = send_call->getArgOperand(0);
	Value *count = send_call->getArgOperand(1);
	Value *datatype = send_call->getArgOperand(2);
	Value *dest = send_call->getArgOperand(3);
	Value *tag = send_call->getArgOperand(4);
	Value *comm = send_call->getArgOperand(5);

//TODO do we need to check if all of those values are accessible from this function?

	auto *SE = analysis_results->getSE(parallel_region->get_function());
//TODO insert it at top of function

// arguments for partitioning

	//TODO something is wrong with the in work implementation of the gatehring of the attributes

	// this is just for testing
	// 8th parameter of static_for_init
	Value *chunk_size_in_parallel =
			parallel_region->get_parallel_for()->init->getArgOperand(8);
	Value *loop_min_in_parallel =
			parallel_region->get_parallel_for()->init->getArgOperand(4);

	// TODO what if cast isnt valid?
	// then abort as no chunksize was given
	auto *chunk_access_min = cast<SCEVAddRecExpr>(min_adress->getStart());
	auto *chunk_access_max = cast<SCEVAddRecExpr>(max_adress->getStart());

	auto *min_of_current_chunk = chunk_access_min->evaluateAtIteration(
			SE->getSCEV(loop_min_in_parallel), *SE);
	auto *min_of_next_chunk = chunk_access_min->evaluateAtIteration(
			SE->getSCEV(chunk_size_in_parallel), *SE);

	auto *max_of_current_chunk = chunk_access_max->evaluateAtIteration(
			SE->getSCEV(loop_min_in_parallel), *SE);
	auto *max_of_next_chunk = chunk_access_max->evaluateAtIteration(
			SE->getSCEV(chunk_size_in_parallel), *SE);

	//TODO do we need chunk size +1?
	//auto*start_of_next_chunk=SE->getAddExpr(SE->getSCEV(loop_min_in_parallel), SE->getSCEV(chunk_size_in_parallel));
	//start_of_next_chunk = SE->getAddExpr(start_of_next_chunk, SE->getConstant(start_of_next_chunk->getType(), 1));
	//start_of_next_chunk->dump();

	Value *A_min = get_scev_value_before_parallel_function(min_of_current_chunk,
			insert_point, parallel_region);

	Value *B_min = get_scev_value_before_parallel_function(min_of_next_chunk,
			insert_point, parallel_region);

	Value *A_max = get_scev_value_before_parallel_function(max_of_current_chunk,
			insert_point, parallel_region);

	Value *B_max = get_scev_value_before_parallel_function(max_of_next_chunk,
			insert_point, parallel_region);

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

	std::vector<Value*> argument_list_with_wrong_types { buf, count, datatype,
			dest, tag, comm, request_ptr, A_min, B_min, A_max, B_max,
			chunk_size, loop_min, loop_max };

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
	IRBuilder<> builder(insert_point);

	return cast<CallInst>(
			builder.CreateCall(mpi_func->partition_sending_op, argument_list,
					"partitions"));
}

CallInst* replace_old_send_with_wait(CallInst *send_call, Value *request_ptr) {
// now we need to replace the send call with the wait

	Instruction *insert_point = get_local_completion_point(send_call);
	IRBuilder<> builder(insert_point);

//TODO set status ignore instead?

	Type *MPI_status_ptr_type =
			mpi_func->mpix_Wait->getFunctionType()->getParamType(1);

	Value *status_ptr = builder.CreateAlloca(
			MPI_status_ptr_type->getPointerElementType(), 0, "mpi_status");

	Value *new_send_call = builder.CreateCall(mpi_func->mpix_Wait, {
			request_ptr, status_ptr });

// and remove the old send call
	send_call->replaceAllUsesWith(new_send_call);
	send_call->eraseFromParent();

	if (auto *wait_call = dyn_cast<CallInst>(insert_point)) {
		if (wait_call->getCalledFunction() == mpi_func->mpi_wait) {
			// we can  not remove a waitall
			// but waiting for MPI_REQUEST_NULL is OK as well, so no need to change it
			//TODO we must ensure the request is actually MPI_REQUEST_NULL and not uninitialized!
			wait_call->replaceAllUsesWith(new_send_call);
			wait_call->eraseFromParent();
		}
	}

	return cast<CallInst>(new_send_call);
}

CallInst* insert_request_free(Instruction *insert_before, Value *request_ptr) {

	IRBuilder<> builder(insert_before);
	return cast<CallInst>(builder.CreateCall(mpi_func->mpix_Request_free, {
			request_ptr }));

}

CallInst* insert_new_fork_call(Instruction *insert_point,
		Microtask *parallel_region, Function *new_parallel_function,
		Value *request_ptr) {
// fork_call
//call void (%struct.ident_t*, i32, void (i32*, i32*, ...)*, ...) @__kmpc_fork_call(%struct.ident_t* nonnull @2, i32 2, void (i32*, i32*, ...)* bitcast (void (i32*, i32*, i32*, i32*)* @.omp_outlined. to void (i32*, i32*, ...)*), i8* %call3, i32* nonnull %rank)
// change the call to the new ompoutlined

	IRBuilder<> builder(insert_point);

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
	return builder.CreateCall(original_fork_call->getCalledFunction(), new_args);
}

// only call if the replacement is actually safe
//TODO refactor to be able to make an assertion?
bool insert_partitioning(Microtask *parallel_region, CallInst *send_call,
		const SCEVAddRecExpr *min_adress, const SCEVAddRecExpr *max_adress) {

	assert(min_adress->isAffine() && max_adress->isAffine());

//TOOD for a static schedule, there should be a better way of getting the chunk_size!

	errs() << "detected possible partitioning for MPI send Operation\n";

// we need to duplicate the original Function to add the MPi Request as an argument

// contains a mapping form all original values to the clone
	ValueToValueMapTy VMap;
	Function *new_parallel_function =
			duplicate_parallel_function_with_added_request(parallel_region,
					VMap);

// need to add a call to signoff_partitions after a loop iteration has finished
	add_partition_signoff_call(VMap, parallel_region, new_parallel_function);

	auto *insert_point = parallel_region->get_fork_call();

	IRBuilder<> builder(
			insert_point->getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrLifetime());
// MPI_Request at the start of the func in the first block
	Value *request_ptr = builder.CreateAlloca(mpi_func->mpix_request_type,
			nullptr, "mpix_request");

	auto *partition_init_call = add_partition_init_call(insert_point,
			request_ptr, parallel_region, send_call, min_adress, max_adress);

// need to reset insert point if code was inserted before insert point but after position of builder
	builder.SetInsertPoint(insert_point);
// start the communication
	CallInst *start_call = cast<CallInst>(
			builder.CreateCall(mpi_func->mpix_Start, { request_ptr }));

	auto *new_fork_call = insert_new_fork_call(insert_point, parallel_region,
			new_parallel_function, request_ptr);

	Function *old_ompoutlined = parallel_region->get_function();
	CallInst *original_fork_call = parallel_region->get_fork_call();
// remove old call
	original_fork_call->replaceAllUsesWith(new_fork_call);// unnecessary as it is c void return anyway
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

//TODO do we need to invalidate the microtask obj now?

	auto *wait_call = replace_old_send_with_wait(send_call, request_ptr);

	auto *request_free_call = insert_request_free(wait_call->getNextNode(),
			request_ptr);

	move_init_and_free_call(partition_init_call, request_free_call,
			new_fork_call);

	assert(is_instruction_before(partition_init_call, start_call));

	return true;
}

