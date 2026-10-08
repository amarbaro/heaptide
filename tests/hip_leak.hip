#include <hip/hip_runtime.h>
int main() {
  void *a, *b, *c;
  hipMalloc(&a, 1 << 20); hipMalloc(&b, 2 << 20); hipMalloc(&c, 4 << 20);
  hipFree(b);  // a (1 MiB) and c (4 MiB) leak: live_bytes must be 5242880
  return 0;
}
