add_library(mpi_communication_partition MODULE
    # List your source files here.
    mpi_communication_partition_pass.cpp
    mpi_functions.h
    mpi_functions.cpp
    implementation_specific.h
    implementation_specific.cpp
    debug.h
    analysis_results.h
    analysis_results.cpp
    Microtask.h
    Microtask.cpp
    helper.cpp
    helper.h
    insert_changes.cpp
    insert_changes.h
    sending_partitioning.h
    sending_partitioning.cpp
    mpi_analysis.h
    mpi_analysis.cpp
)

# if one wants to use mpi
find_package(MPI REQUIRED)
include_directories(${MPI_INCLUDE_PATH})
target_link_libraries(mpi_communication_partition PRIVATE MPI::MPI_C)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -Og -DDEBUG_COMMUNICATION_PARTITION_PASS=1")
# swith off one Warning for release build: some variables are only used for assertions
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O1 -DDEBUG_COMMUNICATION_PARTITION_PASS=0 -Wno-unused-but-set-variable")
#TODO figure out why O2 will result in an segfault in release build!

# Use C++11 to compile our pass (i.e., supply -std=c++11).
target_compile_features(mpi_communication_partition PRIVATE cxx_range_for cxx_auto_type)

# LLVM is (typically) built with no C++ RTTI. We need to match that;
# otherwise, we'll get linker errors about missing RTTI data.
set_target_properties(mpi_communication_partition PROPERTIES
    COMPILE_FLAGS "-fno-rtti -Wall -Wextra -Wno-unused-parameter"
)

