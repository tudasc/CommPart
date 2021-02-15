cmake_minimum_required(VERSION 3.16)# what I use on the cluster, may be set lower

project("MPI Assertion Checking")

# specify LLVM version number
find_package(LLVM 10 REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_subdirectory(dump_ir_pass)
add_subdirectory(mpi_communication_partition)
