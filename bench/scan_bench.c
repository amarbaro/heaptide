// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// Exit-scan worst case (argv[1] blocks, default 1M): live 64 B blocks, each word an interior pointer into a random block.
#include <stdint.h>
#include <stdlib.h>
static int N = 1 << 20;
uint64_t **all;
int main(int argc, char **argv) {
  if (argc > 1) N = atoi(argv[1]);
  all = malloc(N * sizeof *all);
  for (int i = 0; i < N; i++) all[i] = malloc(64);
  uint64_t x = 1;
  for (int i = 0; i < N; i++)
    for (int j = 0; j < 8; j++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; all[i][j] = (uint64_t)all[x % N] + 8; }
  return 0;
}
