// A thread that never allocated through malloc maps 3 pages and turns the middle one into a guard
// (PROT_NONE), then keeps the mapping in a global. Attached, that thread's first unwind is not taken
// in the mmap hook, so the block has no stack: the exit scan must still read it as a mapping (copied
// page by page), not in place, or the guard page faults. Prints "survived" and exits 0 otherwise.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
static char *keep;
static void *work(void *x) {
  char *p = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return x;
  p[0] = 1;
  mprotect(p + 4096, 4096, PROT_NONE);
  keep = p;
  return x;
}
int main(int argc, char **argv) {
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
  printf("%d\n", getpid()); fflush(stdout);
  if (argc > 1) {  // "attach": wait for heaptide
    for (int t = 0; t < 400 && !getenv("HEAPTIDE_ATTACH"); t++) usleep(50000);
    usleep(300000);
  }
  pthread_t t;
  pthread_create(&t, 0, work, 0);
  pthread_join(t, 0);
  puts(keep ? "survived" : "no mapping");
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
