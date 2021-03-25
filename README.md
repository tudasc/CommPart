MPI Communication Partitioning
=======

This clang Pass partitions MPI sending operations among different chunks of an OpenMP for loop. (see Chapter 4 of the upcoming MPI standard \[[MPI20](#ref-mpi2020)\])

Building
-----------
Building the Pass with Cmake is quite straightforward:

``mkdir build; cd build; cmake ..; make -j 4``
You need LLVM/clang version 11.1.

Running
-----------
For running the pass, you need an MPI Implementation built with clang (Tested with mpich 3.3.2).
For convenience, you can use the 'run.sh' script in order to run the transformation.
The code to transform has to include the `correctness-checking-partitioned-impl.h` header file, which provides a naive implementation of the Partitioned operations with built in correctness checking. demo-codes contains some examples.


References
-----------
<table style="border:0px">
<tr>
    <td valign="top"><a name="ref-CommPart20"></a>[CommPart21]</td>
    <td>Jammer, Tim and and Bischof, Christian:
       Automatic Partitioning of MPI operations in MPI+OpenMP applications 2021.</td>
</tr>
<tr>
    <td valign="top"><a name="ref-mpi2020"></a>[MPI20]</td>
    <td>Message Passing Interface Forum:
    <a href=https://www.mpi-forum.org/docs/drafts/mpi-2020-draft-report.pdf>
    MPI: A Message-Passing Interface Standard - Version 4.0 Draft, 2020</td>
</tr>
</table>
