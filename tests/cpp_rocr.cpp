// Planted: the shapes of the ROCm runtime's one-time allocations (a queue ring never freed, two blocks lost in
// __add_to_environ under rocprofiler_register), and leaks that are the program's own: a malloc in a callback run by a function
// named like a HIP API call, and the same __add_to_environ called from the program. The default suppression list drops the
// first three blocks and never the others (glibc's real setenv keeps its strings reachable here, so the leaf is a stand-in).
#include <cstdlib>
namespace rocr { namespace AMD {
struct AqlQueue { char *ring; AqlQueue(); };
AqlQueue::AqlQueue() { ring = new char[72]; }
}}
__attribute__((noinline)) static void *__add_to_environ(size_t n) { void *volatile p = malloc(n); return p; }
namespace rocprofiler_register { namespace common {
__attribute__((noinline)) void set_env() { void *volatile a = __add_to_environ(24); void *volatile b = __add_to_environ(23); (void)a; (void)b; }
}}
extern "C" __attribute__((noinline)) int hipLaunchHostFunc(void (*fn)(void *), void *arg) { fn(arg); return 0; }
__attribute__((noinline)) static void user_cb(void *) { void *volatile p = malloc(777); (void)p; }
__attribute__((noinline)) static void own_env() { void *volatile a = __add_to_environ(31); (void)a; }
int main() {
  delete new rocr::AMD::AqlQueue();  // the object goes, its ring is lost
  rocprofiler_register::common::set_env();
  own_env();
  hipLaunchHostFunc(user_cb, nullptr);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
