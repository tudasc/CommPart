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

#include "correctness-checking-partitioned-impl.h"
#include "mpi.h"

#include <stdio.h>
#include <stdlib.h>

#define SIZE 16
#define PARTITIONS 16
#define TAG 42

void correct_usage() {
  MPIX_Request r;

  int *buffer = (int*) malloc(sizeof(int) * PARTITIONS * SIZE);

  MPIX_Psend_init(buffer, PARTITIONS, SIZE, MPI_INT, 1, TAG, MPI_COMM_WORLD,
                  MPI_INFO_NULL, &r);

  MPIX_Start(&r);

#pragma omp parallel for
  for (int i = 0; i < PARTITIONS; ++i) {
    for (int j = i * SIZE; j < i * SIZE + SIZE; ++j) {

      buffer[j] = i;
    }
    MPIX_Pready(i, &r);
  }

  MPIX_Wait(&r, MPI_STATUS_IGNORE);

  MPIX_Request_free(&r);
}

void correct_usage_with_false_positives() {
  MPIX_Request r;

  int *buffer = (int*) malloc(sizeof(int) * PARTITIONS * SIZE);
  int sum = 0;

  MPIX_Psend_init(buffer, PARTITIONS, SIZE, MPI_INT, 1, TAG, MPI_COMM_WORLD,
                  MPI_INFO_NULL, &r);

  MPIX_Start(&r);

#pragma omp parallel for reduction(+ : sum)
  for (int i = 0; i < PARTITIONS; ++i) {
    for (int j = i * SIZE; j < i * SIZE + SIZE; ++j) {

      buffer[j] = i;
    }
    MPIX_Pready(i, &r);
    for (int j = i * SIZE; j < i * SIZE + SIZE; ++j) {
      // reading is allowed
      sum += buffer[j];
    }
  }

  printf("%d\n", sum);

  MPIX_Wait(&r, MPI_STATUS_IGNORE);

  MPIX_Request_free(&r);
}

void error_usage() {
  MPIX_Request r;

  int *buffer = (int*) malloc(sizeof(int) * PARTITIONS * SIZE);

  MPIX_Psend_init(buffer, PARTITIONS, SIZE, MPI_INT, 1, TAG, MPI_COMM_WORLD,
                  MPI_INFO_NULL, &r);

  MPIX_Start(&r);

  // likely to fail
#pragma omp parallel for
  for (int i = 0; i < PARTITIONS * 2; ++i) {
    if (i < PARTITIONS) {
      for (int j = i * SIZE; j < i * SIZE + SIZE / 2; ++j) {

        buffer[j] = i;
      }
      MPIX_Pready(i, &r);
    } else {

      for (int j = (i - PARTITIONS) * SIZE + SIZE / 2;
           j < (i - PARTITIONS) * SIZE + SIZE; ++j) {

        buffer[j] = i;
      }
    }
  }

  MPIX_Wait(&r, MPI_STATUS_IGNORE);

  MPIX_Request_free(&r);
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == 0) {
    // correct_usage();
    correct_usage_with_false_positives();
    error_usage();
  } else if (rank == 1) {
    MPIX_Request r;
    int *buffer = (int*) malloc(sizeof(int) * PARTITIONS * SIZE);

    MPIX_Precv_init(buffer, 1, (PARTITIONS * SIZE), MPI_INT, 0, TAG,
                    MPI_COMM_WORLD, MPI_INFO_NULL, &r);
    MPIX_Start(&r);
    MPIX_Pready(0, &r);
    MPIX_Wait(&r, MPI_STATUS_IGNORE);

    MPIX_Start(&r);
    MPIX_Pready(0, &r);
    MPIX_Wait(&r, MPI_STATUS_IGNORE);
    MPIX_Request_free(&r);
  }

  MPI_Finalize();
}
