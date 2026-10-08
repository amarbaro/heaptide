// heaptide demo: a request handler that loses one node (and its child) every 10th request.
// cc -O0 -g -o demo examples/demo.c && ./heaptide run -- ./demo && ./heaptide report .work/heaptide
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct node { struct node *next; char payload[120]; } node;
node *cache;                                   /* reachable through a global */
static void handle_request(int i) {
  node *n = malloc(sizeof *n);                 /* lost every 10th request */
  n->next = malloc(64);                        /* child of a lost block */
  if (i % 10) { free(n->next); free(n); }
}
int main(void) {
  struct timespec ms = {0, 2000000};
  cache = malloc(sizeof *cache); cache->next = 0;
  for (int i = 0; i < 1000; i++) { handle_request(i); nanosleep(&ms, 0); }
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
