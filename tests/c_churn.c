// Planted: 1000 temporaries (alloc then free, nothing between) in temp_site;
// burst_site holds 100 x 1 MiB at once (peak consumer), freed after;
// hot_site makes 50000 allocations (hotspot), each freed later in reverse.
#include <stdlib.h>
static void *keep[50000], *big[100];
__attribute__((noinline)) static void temp_site(void) { for (int i = 0; i < 1000; i++) { void *volatile p = malloc(64); free(p); } }
__attribute__((noinline)) static void burst_site(void) { for (int i = 0; i < 100; i++) big[i] = malloc(1 << 20); }
__attribute__((noinline)) static void hot_site(void) { for (int i = 0; i < 50000; i++) keep[i] = malloc(16); }
int main(void) {
  temp_site();
  burst_site();
  for (int i = 0; i < 100; i++) free(big[i]);
  hot_site();
  for (int i = 49999; i >= 0; i--) free(keep[i]);
  return 0;
}
