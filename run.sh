#!/bin/bash

#using openmpi:
#OMPI_CXX=clang++ mpicxx -fopenmp -Xclang -load -Xclang build/experimentpass/libexperimentpass.so  $1

# using mpich:
if [ ${1: -2} == ".c" ]; then
$MPICC -cc=clang -O2 -fopenmp -Xclang -load -Xclang build/mpi_communication_partition/libmpi_communication_partition.so  $1
#$MPICC -cc=clang -O2 -fopenmp -Xclang -load -Xclang build/mpi_communication_partition/libmpi_communication_partition.so  -ftime-report $1
#$MPICC -cc=clang -O2 -fopenmp $1
elif [ ${1: -4} == ".cpp" ]; then
$MPICXX -cxx=clang++ -O2  -fopenmp -Xclang -load -Xclang build/mpi_communication_partition/libmpi_assertion_checker.so  $1
else
echo "Unknown file suffix, use this script with .c or .cpp files"
fi
