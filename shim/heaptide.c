// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// libheaptide: LD_PRELOAD heap tracker for CPU allocators and HIP device/host memory.
// Aggregates in process (per call stack), samples live bytes, classifies unfreed blocks
// at exit (LSan-style reachability), writes HEAPTIDE_OUT/heaptide.<pid>.{bin,maps}.
#define _GNU_SOURCE
#define UNW_LOCAL_ONLY
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libunwind.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define TLS __thread __attribute__((tls_model("initial-exec")))
#define HOOK __attribute__((visibility("default")))
enum { CPU, GPU, HOST, MMAP };
enum { DEPTH = 64, SHARDS = 64, MAXSTACKS = 1 << 24, MAXSAMPLES = 1 << 20 };
enum { REACH = 1 << 24, INDIRECT = 1 << 25, SIZE_SHIFT = 26 };

// One cache line or less per call stack; written to disk as is (56 B).
typedef struct {
  _Atomic uint64_t allocs, bytes, temps, live_n, live_b;
  uint64_t peak_b;
  uint32_t ips_off;
  uint8_t kind, depth, pad[2];
} row_t;
_Static_assert(sizeof(row_t) == 56, "row layout is the file format");

// Open-addressing table, 16 B entries, key 0 = empty. Value of a live block:
// size << 26 | INDIRECT | REACH | stack id (24 bits).
typedef struct { uint64_t k, v; } ent_t;
typedef struct { atomic_flag lk; ent_t *t; size_t cap, n; char pad[32]; } tab_t;

static tab_t live[SHARDS], stacks[SHARDS];
static row_t *rows;
static uint64_t *ips_arena, (*samples)[3];
static _Atomic uint32_t nstacks;
static _Atomic uint64_t ips_used, nsamples, peak_mark;
// Live bytes per kind, one cache line per thread (a shared counter cost most of the hook time with
// 8 threads); readers sum the lines. Signed: a thread may free what another allocated.
enum { SLOTS = 1024 };
typedef struct { uint32_t id, ops; int64_t d[5]; } pend_t;  // allocs, bytes, temps, live_n, live_b
static struct { _Alignas(64) _Atomic int64_t k[8]; pend_t pend[16]; } lslot[SLOTS];
static _Atomic int nslot;
static TLS int my_slot = -1;
static TLS unsigned tick;
static void add_live(int kind, int64_t d) {
  if (my_slot < 0) my_slot = atomic_fetch_add(&nslot, 1);
  atomic_fetch_add_explicit(&lslot[my_slot % SLOTS].k[kind], d, memory_order_relaxed);
}
// Row counters are shared by every thread allocating at that site; each thread batches its changes
// in its slot (16 sites, flushed every 64 updates or on eviction) and the exit handler flushes all.
static void flush(pend_t *e) {
  row_t *r = &rows[e->id];
  r->allocs += e->d[0]; r->bytes += e->d[1]; r->temps += e->d[2]; r->live_n += e->d[3]; r->live_b += e->d[4];
  memset(e->d, 0, sizeof e->d); e->ops = 0;
}
static void bump(uint32_t id, int64_t a, int64_t b, int64_t t, int64_t ln, int64_t lb) {
  if (my_slot < 0) my_slot = atomic_fetch_add(&nslot, 1);
  if (my_slot >= SLOTS) {  // slot shared with another thread: no batching
    row_t *r = &rows[id];
    r->allocs += a; r->bytes += b; r->temps += t; r->live_n += ln; r->live_b += lb;
    return;
  }
  pend_t *e = &lslot[my_slot].pend[id & 15];
  if (e->id != id) { flush(e); e->id = id; }
  e->d[0] += a; e->d[1] += b; e->d[2] += t; e->d[3] += ln; e->d[4] += lb;
  if (++e->ops >= 64) flush(e);
}
static void flush_mine(void) {  // before a peak snapshot: this thread's rows must be current
  for (int j = 0; my_slot >= 0 && my_slot < SLOTS && j < 16; j++)
    if (lslot[my_slot].pend[j].ops) flush(&lslot[my_slot].pend[j]);
}
static void flush_all(void) {
  for (int i = 0, n = nslot < SLOTS ? nslot : SLOTS; i < n; i++)
    for (int j = 0; j < 16; j++) flush(&lslot[i].pend[j]);
}
static uint64_t live_total(int kind) {
  int64_t t = 0;
  for (int i = 0, n = nslot < SLOTS ? nslot : SLOTS; i < n; i++) t += lslot[i].k[kind];
  return t > 0 ? (uint64_t)t : 0;
}
static atomic_flag peak_lk;
static int ready, every_ms = 100, depth = DEPTH;
static uintptr_t self_lo, self_n;  // libheaptide's code: its frames are not part of a stack
static volatile int stop;
static TLS int guard;
static TLS void *last_alloc;
static TLS int in_alloc;  // inside a hooked allocator: the mappings it makes are its own blocks
#define IN(call) ({ in_alloc++; __typeof__(call) v_ = (call); in_alloc--; v_; })

static void *(*r_malloc)(size_t), *(*r_calloc)(size_t, size_t), *(*r_realloc)(void *, size_t);
static void (*r_free)(void *);
static int (*r_posix_memalign)(void **, size_t, size_t);
static void *(*r_aligned_alloc)(size_t, size_t), *(*r_memalign)(size_t, size_t), *(*r_valloc)(size_t);
static void *(*r_dlsym)(void *, const char *);
static void *(*r_mmap)(void *, size_t, int, int, int, off_t), *(*r_mremap)(void *, size_t, size_t, int, ...);
static int (*r_munmap)(void *, size_t);

static void *vm(size_t n) {  // raw syscall: our own memory never passes the mmap hook
  void *p = (void *)syscall(SYS_mmap, 0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
static void lock(atomic_flag *f) { while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) ; }
static void unlock(atomic_flag *f) { atomic_flag_clear_explicit(f, memory_order_release); }

static ent_t *slot(tab_t *s, uint64_t k) {
  for (size_t i = mix(k) & (s->cap - 1);; i = (i + 1) & (s->cap - 1))
    if (s->t[i].k == k || !s->t[i].k) return &s->t[i];
}
static void put(tab_t *s, uint64_t k, uint64_t v) {
  if (!s->t || (s->n + 1) * 2 > s->cap) {
    tab_t o = *s;
    s->cap = o.cap ? o.cap * 2 : 1024; s->t = vm(s->cap * sizeof(ent_t)); s->n = 0;
    for (size_t i = 0; i < o.cap; i++) if (o.t[i].k) { *slot(s, o.t[i].k) = o.t[i]; s->n++; }
    if (o.t) syscall(SYS_munmap, o.t, o.cap * sizeof(ent_t));
  }
  ent_t *e = slot(s, k);
  if (!e->k) { e->k = k; s->n++; }
  e->v = v;
}
static ent_t *get(tab_t *s, uint64_t k) { if (!s->t) return 0; ent_t *e = slot(s, k); return e->k ? e : 0; }
static int del(tab_t *s, uint64_t k, uint64_t *v) {
  ent_t *e = get(s, k);
  if (!e) return 0;
  *v = e->v; e->k = 0; s->n--;
  size_t i = e - s->t;  // backward-shift the probe chain so lookups never stop early
  for (size_t j = (i + 1) & (s->cap - 1); s->t[j].k; j = (j + 1) & (s->cap - 1)) {
    ent_t m = s->t[j]; s->t[j].k = 0; *slot(s, m.k) = m;
  }
  return 1;
}
static tab_t *shard(tab_t *t, uint64_t k) { return &t[mix(k) >> 58]; }

static uint32_t intern(void **ips, int d, int kind) {
  uint64_t h = kind + 1;
  for (int i = 0; i < d; i++) h = mix(h ^ (uint64_t)ips[i]);
  h |= 1;
  tab_t *s = shard(stacks, h);
  lock(&s->lk);
  ent_t *e = get(s, h);
  uint32_t id = e ? (uint32_t)e->v : 0;
  if (!id && (id = atomic_fetch_add(&nstacks, 1) + 1) < MAXSTACKS) {
    uint64_t off = atomic_fetch_add(&ips_used, d);
    memcpy(ips_arena + off, ips, d * sizeof(void *));
    rows[id].ips_off = off; rows[id].depth = d; rows[id].kind = kind;
    put(s, h, id);
  }
  unlock(&s->lk);
  return id < MAXSTACKS ? id : 0;
}

static void peak_check(int force) {
  uint64_t tot = live_total(CPU) + live_total(GPU) + live_total(HOST) + live_total(MMAP), m = peak_mark;
  if (force ? tot < m : tot <= m + m / 100 + 256) return;
  if (atomic_flag_test_and_set(&peak_lk)) return;
  uint32_t n = nstacks < MAXSTACKS ? nstacks : MAXSTACKS - 1;
  for (uint32_t i = 1; i <= n; i++) rows[i].peak_b = rows[i].live_b;
  peak_mark = tot;
  atomic_flag_clear(&peak_lk);
}

__attribute__((always_inline)) static inline uint32_t stack_id(int kind) {  // inlined: one less frame to unwind
  void *buf[DEPTH + 4], **ips = buf;
  int d = unw_backtrace(buf, depth + 4);
  while (d && (uintptr_t)*ips - self_lo < self_n) ips++, d--;  // our own frames
  if (d > depth) d = depth;
  // a stack ends before the first return address it repeats: recursion (an interpreter loop
  // re-entered through C, as CPython does for every import and callback) adds no new site
  for (int i = 1; i < d; i++)
    for (int j = 0; j < i; j++)
      if (ips[j] == ips[i]) { d = i; goto cut; }
cut:;
  return intern(ips, d, kind);
}

static void track(void *p, size_t n, int kind) {
  if (!p || !ready || guard) return;
  guard = 1;
  uint32_t id = stack_id(kind);
  bump(id, 1, n, 0, 1, n);
  add_live(kind, n);
  tab_t *s = shard(live, (uint64_t)p);
  lock(&s->lk); put(s, (uint64_t)p, (uint64_t)n << SIZE_SHIFT | id); unlock(&s->lk);
  last_alloc = p;
  if (!(++tick & 255) || n >= 65536) flush_mine(), peak_check(0);
  guard = 0;
}

static void untrack(void *p) {
  if (!p || !ready || guard) return;
  uint64_t v;
  tab_t *s = shard(live, (uint64_t)p);
  lock(&s->lk); int hit = del(s, (uint64_t)p, &v); unlock(&s->lk);
  if (!hit) return;
  uint32_t id = v & (REACH - 1);
  int64_t n = v >> SIZE_SHIFT;
  bump(id, 0, 0, p == last_alloc, -1, -n);
  add_live(rows[id].kind, -n);
  last_alloc = 0;
}

// Anonymous mappings made outside the hooked allocators: few, so one locked array; a partial
// munmap trims or splits an entry. They join the live table at exit.
typedef struct { uint64_t a, n; uint32_t id; } map_t;
static map_t *mm;
static size_t nmm;
static atomic_flag mm_lk;
static uint64_t pages(size_t n) { return (n + 4095) & ~4095UL; }
static void mm_add(void *p, size_t len, uint32_t id) {  // holds mm_lk
  uint64_t n = pages(len);
  row_t *r = &rows[id];
  r->allocs++; r->bytes += n; r->live_n++; r->live_b += n;
  add_live(MMAP, n);
  if (nmm < (1 << 20)) mm[nmm++] = (map_t){(uint64_t)p, n, id};
}
static int mm_cut(void *p, size_t len) {  // forget [p, p+len) in every entry; holds mm_lk
  uint64_t a = (uint64_t)p, e = a + pages(len);
  int hit = 0;
  for (size_t i = 0; i < nmm; i++) {
    map_t m = mm[i];
    uint64_t lo = a > m.a ? a : m.a, hi = e < m.a + m.n ? e : m.a + m.n;
    if (lo >= hi) continue;
    hit = 1;
    rows[m.id].live_b -= hi - lo; add_live(MMAP, -(int64_t)(hi - lo));
    if (hi < m.a + m.n && nmm < (1 << 20)) { mm[nmm++] = (map_t){hi, m.a + m.n - hi, m.id}; rows[m.id].live_n++; }
    if (lo > m.a) mm[i].n = lo - m.a;
    else { mm[i--] = mm[--nmm]; rows[m.id].live_n--; }
  }
  return hit;
}
static int watch(void) { return ready && !guard && !in_alloc; }

// dlsym may calloc before the real allocators are known: serve those from here.
static char boot[1 << 14];
static size_t boot_used;
static int in_boot(void *p) { return (char *)p >= boot && (char *)p < boot + sizeof boot; }
static void *boot_alloc(size_t n) {
  n = (n + 15) & ~15UL;
  if (boot_used + n > sizeof boot) return 0;
  void *p = boot + boot_used; boot_used += n; return p;
}

static void *next(const char *name) {
  if (!r_dlsym) r_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  return r_dlsym(RTLD_NEXT, name);
}

// Other libraries' constructors can allocate before ours runs: resolve on first use, and serve
// only the allocations made while resolving (dlsym's own calloc) from the static buffer.
static int resolving;
static void resolve(void) {
  if (r_malloc || resolving) return;
  resolving = 1;
  r_calloc = next("calloc"); r_realloc = next("realloc"); r_free = next("free");
  r_posix_memalign = next("posix_memalign"); r_aligned_alloc = next("aligned_alloc");
  r_memalign = next("memalign"); r_valloc = next("valloc");
  r_mmap = next("mmap"); r_munmap = next("munmap"); r_mremap = next("mremap");
  r_malloc = next("malloc");
  resolving = 0;
}

HOOK void *malloc(size_t n) { resolve(); if (!r_malloc) return boot_alloc(n); void *p = IN(r_malloc(n)); track(p, n, CPU); return p; }
HOOK void *calloc(size_t c, size_t n) {
  resolve();
  if (!r_calloc) return boot_alloc(c * n);  // static buffer is zeroed
  void *p = IN(r_calloc(c, n)); track(p, c * n, CPU); return p;
}
HOOK void free(void *p) { if (!p || in_boot(p)) return; resolve(); untrack(p); in_alloc++; r_free(p); in_alloc--; }
HOOK void *realloc(void *p, size_t n) {
  resolve();
  if (in_boot(p)) { void *q = malloc(n); if (q) memcpy(q, p, n); return q; }
  untrack(p);
  void *q = IN(r_realloc(p, n));
  track(q ? q : p, q ? n : 0, CPU);  // failed realloc keeps p alive; size unknown here, kept as 0
  return q;
}
HOOK void *reallocarray(void *p, size_t c, size_t n) {
  size_t t;
  if (__builtin_mul_overflow(c, n, &t)) return 0;
  return realloc(p, t);
}
HOOK int posix_memalign(void **p, size_t a, size_t n) { resolve(); int e = IN(r_posix_memalign(p, a, n)); if (!e) track(*p, n, CPU); return e; }
HOOK void *aligned_alloc(size_t a, size_t n) { resolve(); void *p = IN(r_aligned_alloc(a, n)); track(p, n, CPU); return p; }
HOOK void *memalign(size_t a, size_t n) { resolve(); void *p = IN(r_memalign(a, n)); track(p, n, CPU); return p; }
HOOK void *valloc(size_t n) { resolve(); void *p = IN(r_valloc(n)); track(p, n, CPU); return p; }
HOOK void free_sized(void *p, size_t n) { (void)n; free(p); }
HOOK void free_aligned_sized(void *p, size_t a, size_t n) { (void)a; (void)n; free(p); }

HOOK void *mmap(void *a, size_t n, int prot, int fl, int fd, off_t off) {
  resolve();
  void *p = r_mmap(a, n, prot, fl, fd, off);
  int anon = (fl & MAP_ANONYMOUS) && prot != PROT_NONE;  // PROT_NONE reserves address space only
  if (p == MAP_FAILED || !watch() || !(anon || (fl & MAP_FIXED))) return p;
  guard = 1;
  uint32_t id = anon ? stack_id(MMAP) : 0;
  lock(&mm_lk);
  if (fl & MAP_FIXED) mm_cut(p, n);  // replaces whatever was mapped there
  if (anon) mm_add(p, n, id);
  unlock(&mm_lk);
  peak_check(0);
  guard = 0;
  return p;
}
HOOK void *mmap64(void *, size_t, int, int, int, off_t) __attribute__((alias("mmap")));
HOOK int munmap(void *p, size_t n) {
  resolve();
  if (!watch()) return r_munmap(p, n);
  lock(&mm_lk);  // held across the call: an address freed here may be mapped again by another thread
  int e = r_munmap(p, n);
  if (!e) mm_cut(p, n);
  unlock(&mm_lk);
  return e;
}
HOOK void *mremap(void *p, size_t o, size_t n, int fl, ...) {
  va_list ap; va_start(ap, fl); void *to = va_arg(ap, void *); va_end(ap);
  resolve();
  if (!watch()) return r_mremap(p, o, n, fl, to);
  guard = 1;
  uint32_t id = stack_id(MMAP);
  lock(&mm_lk);
  void *q = r_mremap(p, o, n, fl, to);
  if (q != MAP_FAILED && mm_cut(p, o)) mm_add(q, n, id);  // re-homed at the mremap call, like realloc
  unlock(&mm_lk);
  guard = 0;
  return q;
}

// HIP: the Mojo runtime resolves these through dlsym after dlopen, so dlsym is hooked too.
static void *hip(const char *name) {
  void *f = next(name), *h;
  if (!f && (h = dlopen("libamdhip64.so.7", RTLD_NOW | RTLD_NOLOAD))) f = r_dlsym(h, name);
  return f;
}
#define REAL(args, name) static int(*r) args; if (!r) r = hip(name)
HOOK int hipMalloc(void **p, size_t n) { REAL((void **, size_t), "hipMalloc"); int e = IN(r(p, n)); if (!e) track(*p, n, GPU); return e; }
HOOK int hipMallocAsync(void **p, size_t n, void *s) { REAL((void **, size_t, void *), "hipMallocAsync"); int e = IN(r(p, n, s)); if (!e) track(*p, n, GPU); return e; }
HOOK int hipHostMalloc(void **p, size_t n, unsigned f) { REAL((void **, size_t, unsigned), "hipHostMalloc"); int e = IN(r(p, n, f)); if (!e) track(*p, n, HOST); return e; }
HOOK int hipFree(void *p) { REAL((void *), "hipFree"); untrack(p); return IN(r(p)); }
HOOK int hipHostFree(void *p) { REAL((void *), "hipHostFree"); untrack(p); return IN(r(p)); }
HOOK int hipFreeAsync(void *p, void *s) { REAL((void *, void *), "hipFreeAsync"); untrack(p); return IN(r(p, s)); }

HOOK void *dlsym(void *h, const char *name) {
  static const struct { const char *n; void *f; } ours[] = {
      {"hipMalloc", hipMalloc}, {"hipMallocAsync", hipMallocAsync}, {"hipHostMalloc", hipHostMalloc},
      {"hipFree", hipFree},     {"hipHostFree", hipHostFree},       {"hipFreeAsync", hipFreeAsync}};
  for (size_t i = 0; i < sizeof ours / sizeof *ours; i++)
    if (!strcmp(name, ours[i].n)) return ours[i].f;
  if (!r_dlsym) r_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  return r_dlsym(h, name);
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ULL + t.tv_nsec; }
static void *sampler(void *a) {
  (void)a;
  guard = 1;
  struct timespec d = {every_ms / 1000, (long)(every_ms % 1000) * 1000000};
  while (!stop && nsamples < MAXSAMPLES) {
    uint64_t i = nsamples;
    samples[i][0] = now_ns(); samples[i][1] = live_total(CPU) + live_total(HOST) + live_total(MMAP); samples[i][2] = live_total(GPU);
    nsamples = i + 1;
    nanosleep(&d, 0);
  }
  return 0;
}

// Reachability (LSan port, exact block starts only): roots = writable segments of loaded
// objects; flood through CPU blocks; unreached = leaked; leaked blocks reached from other
// leaked blocks = indirect.
static int heap(uint64_t v) { int k = rows[v & (REACH - 1)].kind; return k == CPU || k == MMAP; }
static uint64_t *work;
static size_t nwork;
static ent_t *find_live(uint64_t v) { return v & 7 ? 0 : get(shard(live, v), v); }
static void scan(const char *b, const char *e, uint64_t bit) {
  for (b = (const char *)(((uintptr_t)b + 7) & ~7UL); b + 8 <= e; b += 8) {
    ent_t *x = find_live(*(const uint64_t *)b);
    // GPU/host blocks are never roots-reachable: the HIP runtime's own tables point at all of them
    if (x && !(x->v & (REACH | INDIRECT)) && heap(x->v)) { x->v |= bit; work[nwork++] = x->k; }
  }
}
// Memory that may hold unreadable pages (thread stacks, mappings) is copied 64 pages per call
// through process_vm_readv instead of read in place; a copy stops at the first page that fails.
// glibc's thread-stack guards are madvise guards inside one merged VMA: a stack (stop = 1) ends
// at its first unreadable page, a mapping skips the page.
static void scan_copy(uint64_t a, uint64_t e, uint64_t bit, int stop) {
  static uint64_t buf[64 * 512];
  struct iovec l = {buf, 0}, r[64];
  for (a &= ~7UL; a < e;) {
    uint64_t b = a;
    int n = 0;
    for (; n < 64 && b < e; n++) {
      uint64_t c = (b | 4095) + 1 < e ? (b | 4095) + 1 : e;
      r[n] = (struct iovec){(void *)b, c - b}; b = c;
    }
    l.iov_len = b - a;
    ssize_t k = process_vm_readv(getpid(), &l, 1, r, n, 0);
    if (k > 0) scan((const char *)buf, (const char *)buf + k, bit);
    if (k == (ssize_t)(b - a)) { a = b; continue; }
    if (stop) return;
    a = ((a + (k > 0 ? k : 0)) | 4095) + 1;
  }
}
static void contents(uint64_t p, uint64_t v, uint64_t bit) {
  int k = rows[v & (REACH - 1)].kind;
  if (k == CPU) scan((char *)p, (char *)p + (v >> SIZE_SHIFT), bit);
  if (k == MMAP) scan_copy(p, p + (v >> SIZE_SHIFT), bit, 0);
}
static void flood(uint64_t bit) {
  while (nwork) { uint64_t p = work[--nwork]; contents(p, find_live(p)->v, bit); }
}
static uintptr_t ld_lo, ld_n;  // the dynamic linker's code
static int root_cb(struct dl_phdr_info *in, size_t sz, void *self) {
  (void)sz;
  if (in->dlpi_addr == (uintptr_t)self) return 0;
  for (int i = 0; i < in->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &in->dlpi_phdr[i];
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && in->dlpi_addr == getauxval(AT_BASE))
      ld_lo = in->dlpi_addr + ph->p_vaddr, ld_n = ph->p_memsz;
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
      char *b = (char *)in->dlpi_addr + ph->p_vaddr;
      scan(b, b + ph->p_memsz, REACH);
      flood(REACH);
    }
  }
  return 0;
}

// Thread roots (LSan: every thread's registers and its stack from SP up). Each other thread is
// parked in a signal handler that saved its registers; a thread that blocks the signal or does
// not answer gives its SP from /proc, without registers.
enum { MAXTHR = 4096 };
typedef struct { pid_t tid; uint64_t sp; greg_t regs[NGREG]; } thr_t;
static thr_t *thr;
static int nthr;
static _Atomic int parked;
static volatile int released;
static ssize_t slurp(const char *path, char *buf, size_t n) {  // raw: no malloc while threads park
  int fd = open(path, O_RDONLY);
  ssize_t k = 0, r;
  while (fd >= 0 && k < (ssize_t)n - 1 && (r = read(fd, buf + k, n - 1 - k)) > 0) k += r;
  if (fd >= 0) close(fd);
  buf[k > 0 ? k : 0] = 0;
  return k;
}
static void park(int sig, siginfo_t *si, void *ctx) {
  (void)sig; (void)si;
  pid_t me = gettid();
  for (int i = 0; i < nthr && !released; i++)
    if (thr[i].tid == me) {
      memcpy(thr[i].regs, ((ucontext_t *)ctx)->uc_mcontext.gregs, sizeof thr[i].regs);
      thr[i].sp = thr[i].regs[REG_RSP];
      parked++;
      struct timespec ms = {0, 1000000};
      while (!released) nanosleep(&ms, 0);
    }
}
static void stop_threads(void) {
  int sig = SIGRTMAX - 2, sent = 0;
  struct sigaction sa = {.sa_sigaction = park, .sa_flags = SA_SIGINFO | SA_RESTART};
  sigfillset(&sa.sa_mask);
  sigaction(sig, &sa, 0);  // never restored: a late delivery must not hit the default (terminate)
  thr = vm(MAXTHR * sizeof(thr_t));
  DIR *d = opendir("/proc/self/task");
  for (struct dirent *e; d && (e = readdir(d)) && nthr < MAXTHR;)
    if (e->d_name[0] != '.' && atoi(e->d_name) != gettid()) thr[nthr++].tid = atoi(e->d_name);
  if (d) closedir(d);
  char path[64], buf[4096];
  for (int i = 0; i < nthr; i++) {
    snprintf(path, sizeof path, "/proc/self/task/%d/status", thr[i].tid);
    char *b = slurp(path, buf, sizeof buf) > 0 ? strstr(buf, "SigBlk:") : 0;
    if (b && !(strtoull(b + 7, 0, 16) >> (sig - 1) & 1) && !syscall(SYS_tgkill, getpid(), thr[i].tid, sig)) sent++;
  }
  struct timespec ms = {0, 1000000};
  for (int t = 0; parked < sent && t < 500; t++) nanosleep(&ms, 0);
}
static uint64_t stack_top(uint64_t sp, const char *maps) {  // end of the readable mapping holding sp
  for (const char *l = maps; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
    char *q;
    uint64_t a = strtoull(l, &q, 16), e = strtoull(q + 1, &q, 16);
    if (a <= sp && sp < e) return q[1] == 'r' ? e : 0;
  }
  return 0;
}
static int scan_threads(void) {
  static char maps[1 << 22];
  char path[64], buf[512];
  int n = 0;
  slurp("/proc/self/maps", maps, sizeof maps);
  // The exiting thread is not scanned: its returned frames leave stale slots (on Ubuntu 24.04 one
  // held c_reach's lost child and turned LSan's 256 B indirect leak into "reachable").
  uint64_t sp, top;
  for (int i = 0; i < nthr; i++) {
    sp = thr[i].sp;
    if (!sp) {  // "nr a1..a6 sp pc" while blocked in a syscall
      snprintf(path, sizeof path, "/proc/self/task/%d/syscall", thr[i].tid);
      char *f = slurp(path, buf, sizeof buf) > 0 ? buf : 0;
      for (int k = 0; f && k < 7; k++) f = strchr(f + 1, ' ');
      sp = f ? strtoull(f + 1, 0, 16) : 0;
    }
    if (!sp || !(top = stack_top(sp, maps))) continue;
    n++;
    scan((const char *)thr[i].regs, (const char *)(thr[i].regs + NGREG), REACH);
    scan_copy(sp, top, REACH, 1);
    flood(REACH);
  }
  return n;
}

static void wr(int fd, const void *p, size_t n) {
  for (const char *c = p; n;) { ssize_t k = write(fd, c, n); if (k <= 0) return; c += k; n -= k; }
}
static void fin(void) {
  if (!ready) return;
  ready = 0; guard = 1; stop = 1;
  peak_check(1);  // growth after the last 1% step
  size_t total = 0;
  for (size_t i = 0; i < nmm; i++) put(shard(live, mm[i].a), mm[i].a, mm[i].n << SIZE_SHIFT | mm[i].id);
  for (int i = 0; i < SHARDS; i++) total += live[i].n;
  work = vm((total + 1) * 8);
  stop_threads();
  flush_all();
  Dl_info me; dladdr((void *)fin, &me);
  dl_iterate_phdr(root_cb, me.dli_fbase);
  // LSan's use_ld_allocations: a block with a linker frame is a root (a thread's DTV is held as
  // dtv + 1 in its TCB, which an exact-start scan misses)
  for (int i = 0; i < SHARDS; i++)
    for (size_t j = 0; j < live[i].cap; j++) {
      ent_t *x = &live[i].t[j];
      row_t *r = &rows[x->v & (REACH - 1)];
      for (int k = 0; x->k && !(x->v & REACH) && r->kind == CPU && k < r->depth; k++)
        if (ips_arena[r->ips_off + k] - ld_lo < ld_n) { x->v |= REACH; work[nwork++] = x->k; }
    }
  flood(REACH);
  int scanned = scan_threads();
  for (int i = 0; i < SHARDS; i++)
    for (size_t j = 0; j < live[i].cap; j++) {
      ent_t *x = &live[i].t[j];
      if (x->k && !(x->v & (REACH | INDIRECT)) && heap(x->v)) {
        contents(x->k, x->v, INDIRECT);
        flood(INDIRECT);
      }
    }
  uint32_t n = nstacks < MAXSTACKS ? nstacks : MAXSTACKS - 1;
  uint64_t *direct = vm((n + 1) * 16), *indirect = direct + n + 1, sum[3] = {0};
  for (int i = 0; i < SHARDS; i++)
    for (size_t j = 0; j < live[i].cap; j++) {
      ent_t *x = &live[i].t[j];
      if (!x->k || (x->v & REACH)) continue;
      uint64_t id = x->v & (REACH - 1), b = x->v >> SIZE_SHIFT;
      if (x->v & INDIRECT) { indirect[id] += b; sum[1] += b; } else { direct[id] += b; sum[0] += b; }
    }
  for (uint32_t i = 1; i <= n; i++) sum[2] += rows[i].temps;
  released = 1;

  const char *dir = getenv("HEAPTIDE_OUT");
  char path[4096];
  snprintf(path, sizeof path, "%s/heaptide.%d.bin", dir ? dir : ".", getpid());
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    uint64_t hdr[8] = {0x4544495450414548ULL /* "HEAPTIDE" */, (uint64_t)getpid(), n, ips_used, nsamples, (uint64_t)every_ms, 0, 0};
    wr(fd, hdr, sizeof hdr);
    wr(fd, ips_arena, ips_used * 8);
    wr(fd, rows + 1, n * sizeof(row_t));
    wr(fd, direct + 1, n * 8);
    wr(fd, indirect + 1, n * 8);
    wr(fd, samples, nsamples * 24);
    close(fd);
    snprintf(path, sizeof path, "%s/heaptide.%d.maps", dir ? dir : ".", getpid());
    int in = open("/proc/self/maps", O_RDONLY), out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buf[1 << 16];
    for (ssize_t k; in >= 0 && out >= 0 && (k = read(in, buf, sizeof buf)) > 0;) wr(out, buf, k);
    if (in >= 0) close(in);
    if (out >= 0) close(out);
  }
  dprintf(2, "heaptide: pid=%d direct=%lu indirect=%lu live_cpu=%lu live_gpu=%lu live_host=%lu live_mmap=%lu temps=%lu stacks=%u threads=%d/%d -> %s/heaptide.%d.bin\n",
          getpid(), sum[0], sum[1], live_total(CPU), live_total(GPU), live_total(HOST), live_total(MMAP), sum[2], n, scanned, nthr,
          dir ? dir : ".", getpid());
}

static int self_cb(struct dl_phdr_info *in, size_t sz, void *self) {
  (void)sz;
  for (int i = 0; in->dlpi_addr == (uintptr_t)self && i < in->dlpi_phnum; i++)
    if (in->dlpi_phdr[i].p_type == PT_LOAD && (in->dlpi_phdr[i].p_flags & PF_X))
      self_lo = in->dlpi_addr + in->dlpi_phdr[i].p_vaddr, self_n = in->dlpi_phdr[i].p_memsz;
  return 0;
}

__attribute__((constructor)) static void init(void) {
  resolve();
  rows = vm((size_t)MAXSTACKS * sizeof(row_t));
  ips_arena = vm((size_t)1 << 33);
  samples = vm((size_t)MAXSAMPLES * 24);
  mm = vm((size_t)sizeof(map_t) << 20);
  const char *e = getenv("HEAPTIDE_EVERY");
  if (e && atoi(e) > 0) every_ms = atoi(e);
  if ((e = getenv("HEAPTIDE_DEPTH")) && atoi(e) > 0 && atoi(e) < DEPTH) depth = atoi(e);
  Dl_info me; dladdr((void *)init, &me);
  dl_iterate_phdr(self_cb, me.dli_fbase);
  unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
  atexit(fin);  // registered first, so it runs after the program's own atexit/static destructors
  ready = 1;
  pthread_t t;
  guard = 1;
  if (!pthread_create(&t, 0, sampler, 0)) pthread_detach(t);
  guard = 0;
}
