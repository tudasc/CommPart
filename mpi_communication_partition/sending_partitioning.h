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
#ifndef MPI_COMMUNICATION_PARTITION_SENDING_PARTITIONING_H_
#define MPI_COMMUNICATION_PARTITION_SENDING_PARTITIONING_H_

#include "llvm/IR/Instructions.h"
#include "Openmp_region.h"

bool handle_fork_call(Microtask *parallel_region, llvm::CallInst *send_call);
bool handle_send_call(llvm::CallInst *send_call);

llvm::Instruction* get_latest_modification_of_pointer(llvm::Value *ptr,
		llvm::Instruction *search_before,
		const std::vector<llvm::Instruction*> exclude_instructions);

#endif /* MPI_COMMUNICATION_PARTITION_SENDING_PARTITIONING_H_ */
