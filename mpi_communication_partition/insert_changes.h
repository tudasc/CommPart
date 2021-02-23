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
#ifndef MPI_COMMUNICATION_PARTITION_INSERT_CHANGES_H_
#define MPI_COMMUNICATION_PARTITION_INSERT_CHANGES_H_
// Handle actual insertion of the changes calls

#include "Openmp_region.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/ScalarEvolution.h"

// return true if modification where done
bool handle_modification_location(llvm::CallInst *send_call,
		llvm::Instruction *last_modification);

// return true if modification where done
bool insert_partitioning(Microtask *parallel_region, llvm::CallInst *send_call,
		const llvm::SCEVAddRecExpr *min_adress, const llvm::SCEVAddRecExpr *max_adress);

#endif /* MPI_COMMUNICATION_PARTITION_INSERT_CHANGES_H_ */
