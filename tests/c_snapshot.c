#include <stdlib.h>
#include <unistd.h>
int main(void) {
  for (int i = 0; i < 40; i++) { void *volatile p = malloc(1024); p = 0; usleep(50000); }  // lose 1 KiB per 50 ms
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
