// Planted: a 64 MiB working set held for the whole run, plus 1 KiB lost every ms for 2 s.
// The ~2 MiB rise is 3% of the mean, under the relative 5% rule: GROWTH passes by default and
// fails with --growth-limit 1 (MiB).
#include <stdlib.h>
#include <time.h>
int main(void) {
  struct timespec ms = {0, 1000000};
  void *volatile big = malloc(64 << 20);
  for (int i = 0; i < 2000; i++) { void *volatile p = malloc(1024); (void)p; nanosleep(&ms, 0); }
  free(big);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
