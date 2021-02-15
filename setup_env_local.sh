#!/bin/bash

#module use /home/tj75qeje/.modulefiles

# load required modules
#ml git cmake gcc llvm mpich valgrind

source ~/clang/init_clang.sh

export CC=clang
export CXX=clang++

export MPI_ROOT=/home/tim/mpich-3.4.1/install
export PATH=$PATH:/home/tim/mpich-3.4.1/install/bin
export CPATH=$CPATH:/home/tim/mpich-3.4.1/install/include
export LIBRARY_PATH=$LIBRARY_PATH:/home/tim/mpich-3.4.1/install/lib
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/tim/mpich-3.4.1/install/lib

# setup path to custom mpiu installation (built with clang)
export MPICC=$(which mpicc)
export MPICXX=$(which mpicxx)


# valgrind include path

export CPATH=$CPATH:/usr/include/valgrind

