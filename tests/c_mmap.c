// Planted anonymous mappings (4 KiB pages): lost 16 KiB; 32 KiB with its 3rd page and last
// 2 pages unmapped, rest (20 KiB, 2 pieces) lost; 4 KiB grown by mremap to 12 KiB, lost;
// 8 KiB held by a global (reachable); 4 KiB unmapped whole. mmap direct = 49152 B.
// malloc(1 MiB) lost: glibc mmaps it internally, must count once, as cpu 1048576.
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#define MAP(n) mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
void *keep;
static void lose(void) {
  void *volatile a = MAP(16384);
  char *b = MAP(32768);
  munmap(b + 8192, 4096);
  munmap(b + 24576, 8192);
  void *volatile c = mremap(MAP(4096), 4096, 12288, MREMAP_MAYMOVE);
  munmap(MAP(4096), 4096);
  void *volatile m = malloc(1 << 20);
  (void)a; (void)c; (void)m;
}
int main(void) {
  keep = MAP(8192);
  lose();
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
