// Planted: pool of 512 x 1 KiB filled in the first 0.3 s, then held while 1.7 s of
// alloc/free churn runs; everything freed at exit (flat after warm-up, no leak).
#include <stdlib.h>
#include <time.h>
int main(void) {
  static void *pool[512];
  struct timespec ms = {0, 1000000};
  for (int i = 0; i < 512; i++) { pool[i] = malloc(1024); if (i % 2) nanosleep(&ms, 0); }
  for (int i = 0; i < 1700; i++) { void *volatile t = malloc(4096); free(t); nanosleep(&ms, 0); }
  for (int i = 0; i < 512; i++) free(pool[i]);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
