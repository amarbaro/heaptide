// glibc keeps free-chunk bins in libc's data; a bin points at a chunk's header, which lies in the last
// 16 B of the block before it. A lost 40 B block (chunk 48, next header at offset 32) followed by a
// binned free chunk looked held by libc, so a leak showed in one run and not in the next.
#include <stdlib.h>
void *guard;  // held: b is not merged into top
int main(void) {
  void *volatile a = malloc(40);
  void *b = malloc(2000);  // past tcache and fastbins
  guard = malloc(64);
  free(b);  // main_arena's unsorted bin now points into a's tail
  a = 0;
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
