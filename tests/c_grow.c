// Planted: 1 KiB lost every millisecond for 2 s (steady growth, ~1 MiB/s).
#include <stdlib.h>
#include <time.h>
int main(void) {
  struct timespec ms = {0, 1000000};
  for (int i = 0; i < 2000; i++) { void *volatile p = malloc(1024); (void)p; nanosleep(&ms, 0); }
  return 0;
}
