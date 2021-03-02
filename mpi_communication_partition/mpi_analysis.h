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

#ifndef MPI_COMMUNICATION_PARTITION_MPI_ANALYSIS_H_
#define MPI_COMMUNICATION_PARTITION_MPI_ANALYSIS_H_

#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include <vector>

std::vector<llvm::CallBase *> get_corresponding_wait(llvm::CallBase *call);
//returns the call where the operation is completed locally
// e.g. the corresponding wait ot the ooperation itself for blocking communication
llvm::Instruction* get_local_completion_point(llvm::CallBase *mpi_call);


#endif /* MPI_COMMUNICATION_PARTITION_MPI_ANALYSIS_H_ */
