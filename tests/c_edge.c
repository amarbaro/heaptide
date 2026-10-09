#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
// Allocator and mapping corner cases (one per argument); each prints what the check reads.
static void *root;
static atomic_int stage;
static void *worker(void *a) {  // holds 63 x 4 KiB (under 64 batched updates) while main peaks
  void *p[63];
  for (int i = 0; i < 63; i++) p[i] = malloc(4096);
  stage = 1;
  while (stage != 2) ;
  for (int i = 0; i < 63; i++) free(p[i]);
  return a;
}
int main(int argc, char **argv) {
  const char *m = argc > 1 ? argv[1] : "";
  if (!strcmp(m, "zero")) free(realloc(malloc(12345), 0));                    // realloc(p, 0) frees p
  if (!strcmp(m, "zerobig")) free(realloc(malloc(2 << 20), 0));               // mmap-backed: freed memory unmapped
  if (!strcmp(m, "errno")) { volatile size_t n = SIZE_MAX; errno = 0; free(reallocarray(0, n, 2)); printf("errno=%d\n", errno); }
  if (!strcmp(m, "dlsym")) printf("hip=%d\n", dlsym(RTLD_DEFAULT, "hipMalloc") != 0);
  if (!strcmp(m, "count")) for (int i = 0; i < 2; i++) { void *p = malloc(64); if (!i) root = p; }  // 1 kept, 1 lost
  if (!strcmp(m, "partial")) {  // the middle page of a reservation moved and grown, then committed: 8 KiB
    char *p = mmap(0, 3 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    p = mremap(p + 4096, 4096, 8192, MREMAP_MAYMOVE);
    mprotect(p, 8192, PROT_READ | PROT_WRITE);
  }
  if (!strcmp(m, "fixed")) {  // 4 KiB moved over half of an 8 KiB mapping: 8 KiB stay mapped
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *q = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mremap(p, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, q);
  }
  if (!strcmp(m, "peak")) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    while (stage != 1) ;
    void *volatile big = malloc(1 << 20);
    stage = 2;
    pthread_join(t, 0);
    free(big);
  }
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
