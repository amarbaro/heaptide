// Planted: reachable growth, no leak. g_cache -> table (make_table) -> 2000 x 4 KiB entries
// (add_entry), all held at exit. The report must name the holder chain: add_entry is held by
// make_table, which is held by the global g_cache. With argv[1] the table comes from alt_table.
#include <stdlib.h>
struct table { void *slot[2000]; };
static struct table *g_cache;
__attribute__((noinline)) static struct table *make_table(void) { return calloc(1, sizeof(struct table)); }
__attribute__((noinline)) static struct table *alt_table(void) { return calloc(1, sizeof(struct table)); }
__attribute__((noinline)) static void add_entry(int i) { g_cache->slot[i] = malloc(4096); }
int main(int argc, char **argv) {
  (void)argv;
  g_cache = argc > 1 ? alt_table() : make_table();
  for (int i = 0; i < 2000; i++) add_entry(i);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
