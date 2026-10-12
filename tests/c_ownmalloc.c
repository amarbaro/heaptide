// heaptide attach on a program that exports its own malloc/free (as Chromium's PartitionAlloc shim and
// jemalloc builds do): a tagged header in front of a libc block, and free aborts on a block without it.
// 200 blocks from before attach are freed after it; 200 more strdup calls (libc -> GOT malloc) after
// it, half kept. Prints its pid, then "survived"; exit 0.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
static void *(*lm)(size_t);
static void (*lf)(void *);
static void init(void) { if (!lm) lm = dlsym(RTLD_NEXT, "malloc"), lf = dlsym(RTLD_NEXT, "free"); }
void *malloc(size_t n) { init(); size_t *p = lm(n + 16); if (!p) return 0; p[0] = 0xC0FFEE; p[1] = n; return p + 2; }
void free(void *q) {
  if (!q) return;
  init();
  size_t *p = (size_t *)q - 2;
  if (p[0] != 0xC0FFEE) { fprintf(stderr, "BAD FREE %p\n", q); abort(); }
  lf(p);
}
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *q, size_t n) {
  void *p = malloc(n);
  if (q && p) { size_t o = ((size_t *)q)[-1]; memcpy(p, q, o < n ? o : n); free(q); }
  return p;
}
int main(void) {
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
  void *old[200];
  for (int i = 0; i < 200; i++) old[i] = strdup("allocated before attach");  // libc's strdup: malloc through its GOT
  printf("%d\n", getpid()); fflush(stdout);
  for (int t = 0; t < 400 && !getenv("HEAPTIDE_ATTACH"); t++) usleep(50000);
  usleep(300000);
  for (int i = 0; i < 200; i++) free(old[i]);
  for (int i = 0; i < 200; i++) { char *s = strdup("after"); if (i % 2) free(s); }
  puts("survived");
  return getenv("HEAPTIDE_ATTACH") ? 0 : 3;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
