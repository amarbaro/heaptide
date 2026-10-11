// heaptide diff: the base build leaks 10 x 1000 B at lose_a and peaks 1 MiB at buffer; -DEXTRA adds
// 10 x 500 B at lose_b (NEW), makes buffer 4 MiB (GROWN) and moves lose_a down a line (same site).
// Ten blocks per site: a stale copy of one pointer cannot hide the site from the exit scan.
#include <stdlib.h>
#include <string.h>
__attribute__((noinline)) static void lose_b(void) { void *volatile p = malloc(500); (void)p; }
#ifdef EXTRA
static int pad;
#endif
__attribute__((noinline)) static void lose_a(void) { void *volatile p = malloc(1000); (void)p; }
__attribute__((noinline)) static void buffer(size_t n) { char *volatile b = malloc(n); memset(b, 1, n); free(b); }
int main(void) {
  for (int i = 0; i < 10; i++) lose_a();
#ifdef EXTRA
  for (int i = 0; i < 10; i++) lose_b();
  buffer(4 << 20);
#else
  buffer(1 << 20);
#endif
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
