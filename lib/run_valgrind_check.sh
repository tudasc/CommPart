#!/bin/bash

if [ -z ${NUMRANKS+x} ]; then
# default value or use env var instead
NUMRANKS=2
fi

DELETE_REPORT=1

mpirun -n $NUMRANKS valgrind --tool=memcheck --error-limit=no --xml=yes --xml-file=valgrind.%q{PMI_RANK}.out.xml $1

OVERALL_EXIT=0

for i in $(seq 0 $(( NUMRANKS-1)) ); do 

echo "Rank $i:"
python3 check_valgrind_output.py valgrind.${i}.out.xml
EXITCODE=$?

if [ "$EXITCODE" -ne "0" ]; then
OVERALL_EXIT=$EXITCODE
fi

if [ "$DELETE_REPORT" -eq "1" ]; then
rm valgrind.${i}.out.xml
fi
done

exit $OVERALL_EXIT
