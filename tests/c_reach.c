// Planted: global -> A(64) -> A1(32) reachable; lost B(128) -> C(256): B direct, C indirect.
#include <stdlib.h>
void **keep;
static void lose(void) {
  void **b = malloc(128);
  b[0] = malloc(256);
}
int main(void) {
  keep = malloc(64);
  keep[0] = malloc(32);
  lose();
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
