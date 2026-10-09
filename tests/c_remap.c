#define _GNU_SOURCE
#include <sys/mman.h>
char *kept;
static char *commit(char *at) { mprotect(at, 1 << 20, PROT_READ | PROT_WRITE); return at; }
int main(void) {
  // a reservation can only be moved while it is one mapping, before any part is committed
  char *r = mmap(0, 4 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  r = mremap(r, 4 << 20, 64 << 20, MREMAP_MAYMOVE);
  if (r == MAP_FAILED) return 1;
  kept = commit(r + (1 << 20));   // reachable
  commit(r + (32 << 20));         // lost, in the grown part
  r = 0;
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
