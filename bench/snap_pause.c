// Longest stall the program sees while heaptide snapshots it: 1M live blocks, then a busy loop
// that reports its largest gap between iterations. Drive: `heaptide run -- snap_pause` then
// `heaptide snapshot DIR` while it runs.
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1000000;
  void **keep = malloc(n * sizeof *keep);
  for (int i = 0; i < n; i++) keep[i] = malloc(32);
  fprintf(stderr, "ready\n");
  double end = now() + 4, last = now(), gap = 0;
  while (last < end) { double t = now(); if (t - last > gap) gap = t - last; last = t; }
  struct rusage u; getrusage(RUSAGE_SELF, &u);
  printf("max_gap_ms %.1f rss_mb %ld\n", gap * 1e3, u.ru_maxrss / 1024);
  return keep[0] == 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
