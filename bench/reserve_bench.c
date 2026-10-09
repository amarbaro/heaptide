// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// Reserve 1 GiB PROT_NONE, then commit and decommit 64 KiB chunks 16k times (an arena's pattern).
#include <sys/mman.h>
int main(void) {
  char *r = mmap(0, 1UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  for (unsigned i = 0; i < 16384; i++) {
    char *c = r + (i % 16384) * 65536UL;
    mprotect(c, 65536, PROT_READ | PROT_WRITE); c[0] = 1;
    mprotect(c, 65536, PROT_NONE);
  }
  munmap(r, 1UL << 30);
  return 0;
}
