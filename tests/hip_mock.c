// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// A fake libamdhip64 whose hipFree fails: a failed free must leave the block live.
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#ifdef MOCK_LIB
int hipMalloc(void **p, size_t n) { *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return *p == MAP_FAILED; }
int hipFree(void *p) { (void)p; return 1; }
#else
int hipMalloc(void **p, size_t n);
int hipFree(void *p);
int main(void) { void *p; hipMalloc(&p, 4096); printf("hipFree=%d\n", hipFree(p)); return 0; }
#endif
