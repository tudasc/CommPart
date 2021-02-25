#include "helper.h"
#include "analysis_results.h"
#include "debug.h"

#include <mpi.h>

using namespace llvm;

std::vector<User *> get_function_users(Module &M, StringRef name)
{
    std::vector<User *> func_users;
    if (Function *func = M.getFunction(name))
    {
        for (auto *user : func->users())
        {
            func_users.push_back(user);
        }
    }
    return func_users;
}

int get_pointer_depth(Type *type)
{
    int depth = 0;

    while (type->isPointerTy())
    {
        depth++;
        type = type->getPointerElementType();
    }

    return depth;
}

int get_pointer_depth(Value *value)
{
    Type *type = value->getType();

    return get_pointer_depth(type);
}

int get_mpi_datatype(Type *type)
{
    while (type->isPointerTy())
    {
        type = type->getPointerElementType();
    }

    if (type->isIntegerTy(32))
    {
        return MPI_INT;
    }
    else if (type->isIntegerTy(64))
    {
        return MPI_LONG_LONG;
    }
    else if (type->isFloatTy())
    {
        return MPI_FLOAT;
    }
    else if (type->isDoubleTy())
    {
        return MPI_DOUBLE;
    }
    else if (type->isIntegerTy(8))
    {
        return MPI_CHAR;
    }
    // if MPI_BYTE Is returned, caller should use get_size_in_Bytes to determine the size
    // of buffer
    return MPI_BYTE;
}

int get_mpi_datatype(Value *value)
{
    Type *type = nullptr;

    // If it is a shared array
    if (value->getType()->getPointerElementType()->isArrayTy())
    {
        type = value->getType()->getPointerElementType()->getArrayElementType();
    }
    // If it is a pointer with depth > 1
    else if (value->getType()->getPointerElementType()->isPointerTy())
    {
        type = value->getType()->getPointerElementType();
        while (type->isPointerTy())
        {
            type = type->getPointerElementType();
        }
    }
    // If it is a shared single value
    else
    {
        type = value->getType()->getPointerElementType();
    }
    return get_mpi_datatype(type);
}

size_t get_size_in_Byte(llvm::Module &M, llvm::Value *value)
{
    Type *type = nullptr;

    // If it is a shared array
    if (value->getType()->getPointerElementType()->isArrayTy())
    {
        type = value->getType()->getPointerElementType()->getArrayElementType();
    }
    // If it is a pointer with depth > 1
    else if (value->getType()->getPointerElementType()->isPointerTy())
    {
        type = value->getType()->getPointerElementType();
        while (type->isPointerTy())
        {
            type = type->getPointerElementType();
        }
    }
    // If it is a shared single value
    else
    {
        type = value->getType()->getPointerElementType();
    }
    return get_size_in_Byte(M, type);
}

size_t get_size_in_Byte(llvm::Module &M, llvm::Type *type)
{
    DataLayout *TD = new DataLayout(&M);
    return TD->getTypeAllocSize(type);
}


//TODO this function needs the PdomTree ananlyis. Maybe move it to analysis instead of helper funcitons?
// get last instruction  in the sense that the return value post dominates all other instruction in the given list
// nullptr if not found
	Instruction* get_last_instruction(std::vector<Instruction*> inst_list) {

		if (inst_list.size() == 0) {
			return nullptr;
		}

		Debug(
				// no nullptrs are allowed
		for (auto* i : inst_list){assert(i!=nullptr);}
		)

		Instruction *current_instruction = inst_list[0];
		bool is_viable = true;
		auto *Pdomtree = analysis_results->getPostDomTree(
				current_instruction->getFunction());
		auto *Domtree = analysis_results->getDomTree(
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
						&& !Domtree->dominates(current_instruction, i)
						&& !Pdomtree->dominates(current_instruction, i) ) {
					// found a non dominate relation: abort
					Debug(
					errs() << "could not analyze relation between \n";
					current_instruction->dump();
					i->dump();)

					return nullptr;
				}
			}
			// successfully passed test above
			return current_instruction;

		}

	}
