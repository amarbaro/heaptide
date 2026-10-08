// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// 8 threads x 1M malloc(16..4096)/free pairs, 10K long-lived blocks (freed at end).
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
static void *work(void *arg) {
  uint64_t x = (uintptr_t)arg * 0x9e3779b97f4a7c15ULL + 1;
  void *keep[1250];
  for (int i = 0; i < 1250; i++) keep[i] = malloc(64);
  for (int i = 0; i < 1000000; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    void *volatile p = malloc(16 + x % 4081);
    free(p);
  }
  for (int i = 0; i < 1250; i++) free(keep[i]);
  return 0;
}
int main(void) {
  pthread_t t[8];
  for (long i = 0; i < 8; i++) pthread_create(&t[i], 0, work, (void *)(i + 1));
  for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
  return 0;
}
