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

#ifndef COMMPART_MPI_FUNCTIONS_H_
#define COMMPART_MPI_FUNCTIONS_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

#include <set>

class MpiFunctions {
public:
  llvm::Function *mpi_init = nullptr;
  llvm::Function *mpi_finalize = nullptr;

  llvm::Function *mpi_send = nullptr;
  llvm::Function *mpi_Bsend = nullptr;
  llvm::Function *mpi_Ssend = nullptr;
  llvm::Function *mpi_Rsend = nullptr;
  llvm::Function *mpi_Isend = nullptr;
  llvm::Function *mpi_Ibsend = nullptr;
  llvm::Function *mpi_Issend = nullptr;
  llvm::Function *mpi_Irsend = nullptr;

  llvm::Function *mpi_Sendrecv = nullptr;

  llvm::Function *mpi_recv = nullptr;
  llvm::Function *mpi_Irecv = nullptr;

  llvm::Function *mpi_test = nullptr;
  llvm::Function *mpi_wait = nullptr;
  llvm::Function *mpi_waitall = nullptr;
  llvm::Function *mpi_buffer_detach = nullptr;

  llvm::Function *mpi_barrier = nullptr;
  llvm::Function *mpi_allreduce = nullptr;
  llvm::Function *mpi_Ibarrier = nullptr;
  llvm::Function *mpi_Iallreduce = nullptr;

  // self implemented functions
  llvm::Function *mpix_Psend_init = nullptr;
  llvm::Function *mpix_Precv_init = nullptr;
  llvm::Function *mpix_Pready = nullptr;
  llvm::Function *mpix_Pready_range = nullptr;
  llvm::Function *mpix_Start = nullptr;
  llvm::Function *mpix_Wait = nullptr;
  llvm::Function *mpix_Request_free = nullptr;

  // library funcs to handle the partitioning
  llvm::Function *partition_sending_op = nullptr;
  llvm::Function *signoff_partitions_after_loop_iter = nullptr;

private:
  // maybe i cna delete this?
  std::set<llvm::Function *>
      conflicting_functions; // may result in a conflict for msg overtaking
  std::set<llvm::Function *>
      sync_functions; // will end the conflicting timeframe (like a barrier)
  std::set<llvm::Function *>
      unimportant_functions; // no implications for msg overtaking



public:

  llvm::Type* mpix_request_type = nullptr;

  MpiFunctions(llvm::Module &M);
  ~MpiFunctions(){};

  bool is_mpi_used();
  bool is_mpi_call(llvm::CallBase *call);
  bool is_mpi_function(llvm::Function *f);

  bool is_send_function(llvm::Function *f);
  bool is_recv_function(llvm::Function *f);

  std::vector<llvm::Function *> get_used_send_functions(){
  	return _send_functions;
  }
  std::vector<llvm::Function *> get_used_recv_functions(){
  	return _recv_functions;
  }
  std::vector<llvm::Function *> get_used_send_and_recv_functions(){
  	return _send_recv_functions;
  }
private:
  std::vector<llvm::Function *> _send_functions;
  std::vector<llvm::Function *> _send_recv_functions;
  std::vector<llvm::Function *> _recv_functions;
};

// will be managed by main
extern MpiFunctions *mpi_func;

#endif /* MACH_MPI_FUNCTIONS_H_ */
