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

#ifndef COMMPART_DEBUG_H
#define COMMPART_DEBUG_H

//#define DEBUG_COMMUNICATION_PARTITION_PASS 1

#if DEBUG_COMMUNICATION_PARTITION_PASS == 1
#define Debug(x) x
#else
#define Debug(x)
#endif

// utility makro for debugging:
#define ASK_TO_CONTINIUE                                                                      \
    int ask_for = 0;                                                                          \
    printf("Continiue? (0 to abort):");                                                       \
    scanf("%d", &ask_for);                                                                    \
    if (!ask_for)                                                                             \
    {                                                                                         \
        printf("Aborting\n");                                                                 \
        exit(0);                                                                              \
    }

#endif
