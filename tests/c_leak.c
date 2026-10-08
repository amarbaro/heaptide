// Planted: 100 x 1000 B lost in a loop + 5 B lost once = 100005 B direct leak.
#include <stdlib.h>
static void lose(size_t n) { void *volatile p = malloc(n); (void)p; }
int main(void) {
  for (int i = 0; i < 100; i++) lose(1000);
  lose(5);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
