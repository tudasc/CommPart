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

#include "helper.h"
#include "analysis_results.h"
#include "debug.h"

#include <mpi.h>

using namespace llvm;

std::vector<User*> get_function_users(Module &M, StringRef name) {
	std::vector<User*> func_users;
	if (Function *func = M.getFunction(name)) {
		for (auto *user : func->users()) {
			func_users.push_back(user);
		}
	}
	return func_users;
}

int get_pointer_depth(Type *type) {
	int depth = 0;

	while (type->isPointerTy()) {
		depth++;
		type = type->getPointerElementType();
	}

	return depth;
}

int get_pointer_depth(Value *value) {
	Type *type = value->getType();

	return get_pointer_depth(type);
}

int get_mpi_datatype(Type *type) {
	while (type->isPointerTy()) {
		type = type->getPointerElementType();
	}

	if (type->isIntegerTy(32)) {
		return MPI_INT;
	} else if (type->isIntegerTy(64)) {
		return MPI_LONG_LONG;
	} else if (type->isFloatTy()) {
		return MPI_FLOAT;
	} else if (type->isDoubleTy()) {
		return MPI_DOUBLE;
	} else if (type->isIntegerTy(8)) {
		return MPI_CHAR;
	}
	// if MPI_BYTE Is returned, caller should use get_size_in_Bytes to determine the size
	// of buffer
	return MPI_BYTE;
}

int get_mpi_datatype(Value *value) {
	Type *type = nullptr;

	// If it is a shared array
	if (value->getType()->getPointerElementType()->isArrayTy()) {
		type = value->getType()->getPointerElementType()->getArrayElementType();
	}
	// If it is a pointer with depth > 1
	else if (value->getType()->getPointerElementType()->isPointerTy()) {
		type = value->getType()->getPointerElementType();
		while (type->isPointerTy()) {
			type = type->getPointerElementType();
		}
	}
	// If it is a shared single value
	else {
		type = value->getType()->getPointerElementType();
	}
	return get_mpi_datatype(type);
}

size_t get_size_in_Byte(llvm::Module &M, llvm::Value *value) {
	Type *type = nullptr;

	// If it is a shared array
	if (value->getType()->getPointerElementType()->isArrayTy()) {
		type = value->getType()->getPointerElementType()->getArrayElementType();
	}
	// If it is a pointer with depth > 1
	else if (value->getType()->getPointerElementType()->isPointerTy()) {
		type = value->getType()->getPointerElementType();
		while (type->isPointerTy()) {
			type = type->getPointerElementType();
		}
	}
	// If it is a shared single value
	else {
		type = value->getType()->getPointerElementType();
	}
	return get_size_in_Byte(M, type);
}

size_t get_size_in_Byte(llvm::Module &M, llvm::Type *type) {
	DataLayout *TD = new DataLayout(&M);
	return TD->getTypeAllocSize(type);
}


// true if at least one value in alias_list may alisa with v
bool at_least_one_may_alias(llvm::Function* f,llvm::Value* v,const std::vector<llvm::Value*> alias_list){
	auto* AA = analysis_results->getAAResults(f);
	for (auto* al: alias_list){
		if (!AA->isNoAlias(v, al)){
			return true;
		}
	}

	// no may alias found
	return false;
}
