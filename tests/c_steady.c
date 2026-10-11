// Steady state: 1000 live blocks, one replaced per iteration (random size, two call sites) for
// 3 s. The program's own live set is flat, so growth from t=1 s to t=3 s is heaptide's: VmRSS for
// memory it touches, VmSize for mappings it never touches. Prints "growth_kb RSS SIZE" on stdout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static long kb(const char *key) {
  char l[256]; long kb = -1; FILE *f = fopen("/proc/self/status", "r");
  while (f && fgets(l, sizeof l, f)) if (!strncmp(l, key, strlen(key))) kb = atol(l + strlen(key));
  if (f) fclose(f);
  return kb;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
__attribute__((noinline)) static void *site_a(size_t n) { return malloc(n); }
__attribute__((noinline)) static void *site_b(size_t n) { return calloc(1, n); }
int main(void) {
  static void *live[1000];
  unsigned s = 1;
  long rss = -1, size = -1;
  double t0 = now();
  for (unsigned long i = 0;; i++) {
    s = s * 1103515245 + 12345;
    unsigned k = s % 1000; size_t n = 16 + (s >> 8) % 4096;
    free(live[k]); live[k] = (i & 1) ? site_a(n) : site_b(n);
    double t = now() - t0;
    if (rss < 0 && t > 1) rss = kb("VmRSS:"), size = kb("VmSize:");
    if (t > 3) break;
  }
  printf("growth_kb %ld %ld\n", kb("VmRSS:") - rss, kb("VmSize:") - size);
  for (int k = 0; k < 1000; k++) free(live[k]);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
