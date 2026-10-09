#include <sys/mman.h>
char *kept;
static char *commit(char *at) { mprotect(at, 1 << 20, PROT_READ | PROT_WRITE); return at; }
int main(void) {
  char *r = mmap(0, 16 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  kept = commit(r + (4 << 20));               // reachable
  mprotect(commit(r + (8 << 20)), 1 << 20, PROT_NONE);  // committed, then decommitted
  commit(r + (12 << 20));                     // lost
  madvise(commit(r + (14 << 20)), 1 << 20, MADV_DONTNEED);  // committed, then returned
  char *q = commit(r + (10 << 20)); madvise(q, 1 << 20, MADV_DONTNEED); q[4096] = 1; q = 0;  // returned, reused by a write, lost
  r = 0;
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
