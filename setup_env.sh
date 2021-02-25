#!/bin/bash

#module use /home/tj75qeje/.modulefiles

# load required modules
ml git cmake gcc llvm/debug mpich valgrind

export CC=clang
export CXX=clang++

#export PATH=$PATH:/home/tj75qeje/mpich_with_clang_install/bin
#export CPATH=$CPATH:/home/tj75qeje/mpich_with_clang_install/include
#export LIBRARY_PATH=$LIBRARY_PATH:/home/tj75qeje/mpich_with_clang_install/lib
#export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/tj75qeje/mpich_with_clang_install/lib

# setup path to custom mpiu installation (built with clang)
#export MPICC=/home/tj75qeje/mpich_with_clang_install/bin/mpicc
#export MPICXX=/home/tj75qeje/mpich_with_clang_install/bin/mpicxx

# using mpich:

# include path

export CPATH=$CPATH:/home/tj75qeje/mpi-partitioned-communication/lib

# use 4 threads as standard
export OMP_NUM_THREADS=4

