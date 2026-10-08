// Planted: long setup (64 MiB in 16 x 4 MiB steps over ~0.65 s, a third of the run), then
// 1.5 s of flat alloc/free churn; everything freed at exit (steady after setup, no leak).
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(void) {
  static void *pool[16];
  struct timespec ms = {0, 1000000}, step = {0, 40000000};
  for (int i = 0; i < 16; i++) { pool[i] = malloc(4 << 20); memset(pool[i], 1, 4 << 20); nanosleep(&step, 0); }
  for (int i = 0; i < 1500; i++) { void *volatile t = malloc(4096); free(t); nanosleep(&ms, 0); }
  for (int i = 0; i < 16; i++) free(pool[i]);
  return 0;
}
