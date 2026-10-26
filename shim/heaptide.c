// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// libheaptide: LD_PRELOAD heap tracker for CPU allocators and HIP device/host memory.
// Aggregates in process (per call stack), samples live bytes, classifies unfreed blocks
// at exit (LSan-style reachability), writes HEAPTIDE_OUT/heaptide.<pid>.{bin,maps}.
#define _GNU_SOURCE
#define UNW_LOCAL_ONLY
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define TLS __thread __attribute__((tls_model("initial-exec")))
#define HOOK __attribute__((visibility("default")))
#if __has_attribute(musttail)  // GCC 15+, clang; older GCC still tail-calls at -O2
#define MUSTTAIL __attribute__((musttail))
#else
#define MUSTTAIL
#endif
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
static uint32_t maxstacks = MAXSTACKS;  // smaller under an address-space limit (ulimit -v)
static uint64_t ips_cap = (uint64_t)1 << 30;  // frames
// Live bytes per kind, one cache line per thread (a shared counter cost most of the hook time with
// 8 threads); readers sum the lines. Signed: a thread may free what another allocated.
enum { SLOTS = 1024 };
typedef struct { uint32_t id, ops; int64_t d[5]; } pend_t;  // allocs, bytes, temps, live_n, live_b
static struct { _Alignas(64) _Atomic int64_t k[8]; pend_t pend[16]; } lslot[SLOTS];
static _Atomic int nslot;
static TLS int my_slot = -1;
static TLS unsigned tick;
// --gpu-timeline (1 sites, 2 streams, 4 frag): GPU/HOST rows skip the batch so the sampler reads
// them current; each change is one {sample << 32 | id, live bytes} entry in tl
static int tl_parts;
static TLS uintptr_t stream_tag;  // hipMallocAsync's stream, interned as one more frame
static uint32_t gids[4096];
static _Atomic uint32_t ngids;
static uint64_t (*tl)[2], ntl, frag_base = ~0ULL;
static void *vm(size_t n);
// A thread's first allocation takes a slot and a signal stack: its stack overflow gets a report.
static void new_thread(void) {
  my_slot = atomic_fetch_add(&nslot, 1);
  stack_t a;
  if (!sigaltstack(0, &a) && (a.ss_flags & SS_DISABLE) && (a.ss_sp = vm(1 << 16))) sigaltstack(&(stack_t){a.ss_sp, 0, 1 << 16}, 0);
}
static void add_live(int kind, int64_t d) {
  if (my_slot < 0) new_thread();
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
  if (my_slot < 0) new_thread();
  if (my_slot >= SLOTS || rows[id].kind == GPU || rows[id].kind == HOST) {  // shared slot, or a device row: read current (timeline, py holders)
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
static TLS uintptr_t last_alloc;  // stored inverted: a scan of this thread's TLS must not see a pointer
static TLS int in_alloc;  // inside a hooked allocator: the mappings it makes are its own blocks
#define IN(call) ({ in_alloc++; __typeof__(call) v_ = (call); in_alloc--; v_; })

static void *(*r_malloc)(size_t), *(*r_calloc)(size_t, size_t), *(*r_realloc)(void *, size_t);
static void (*r_free)(void *);
static int (*r_posix_memalign)(void **, size_t, size_t);
static void *(*r_aligned_alloc)(size_t, size_t), *(*r_memalign)(size_t, size_t), *(*r_valloc)(size_t);
static void *(*r_dlsym)(void *, const char *);
static void *(*r_mmap)(void *, size_t, int, int, int, off_t), *(*r_mremap)(void *, size_t, size_t, int, ...);
static int (*r_munmap)(void *, size_t), (*r_mprotect)(void *, size_t, int), (*r_madvise)(void *, size_t, int);

static void *vm(size_t n) {  // raw syscall: our own memory never passes the mmap hook
  void *p = (void *)syscall(SYS_mmap, 0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}
// The big arenas, touched from their start as they fill: under THP "always" each first touch was a
// 2 MiB page (sleep: +7.2 MB RSS, now +1.1 MB). Hash tables and scan arrays keep huge pages (TLB).
static void *cold(size_t n) { void *p = vm(n); if (p) syscall(SYS_madvise, p, n, MADV_NOHUGEPAGE); return p; }
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
static void lock(atomic_flag *f) { while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) ; }
static void unlock(atomic_flag *f) { atomic_flag_clear_explicit(f, memory_order_release); }

static ent_t *slot(tab_t *s, uint64_t k) {
  for (size_t i = mix(k) & (s->cap - 1);; i = (i + 1) & (s->cap - 1))
    if (s->t[i].k == k || !s->t[i].k) return &s->t[i];
}
static void put(tab_t *s, uint64_t k, uint64_t v) {
  ent_t *t;
  if ((!s->t || (s->n + 1) * 2 > s->cap) && (t = vm((s->cap ? s->cap * 2 : 1024) * sizeof(ent_t)))) {
    tab_t o = *s;
    s->cap = o.cap ? o.cap * 2 : 1024; s->t = t; s->n = 0;
    for (size_t i = 0; i < o.cap; i++) if (o.t[i].k) { *slot(s, o.t[i].k) = o.t[i]; s->n++; }
    if (o.t) syscall(SYS_munmap, o.t, o.cap * sizeof(ent_t));
  }
  if (!s->t || s->n + 1 >= s->cap) return;  // out of address space: the block goes untracked
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

// A stack id is found by a 64-bit hash of the frames. The entry also holds a second, independent
// 32-bit hash; a hit whose check hash differs is another stack and takes the next key, so sites
// merge only if 96 bits collide. (Comparing the frames themselves doubled malloc's cost.)
// Each thread keeps the last 64 stacks it saw (ids never change once given): a hit skips the
// shared table's lock, which every thread allocating at one site used to queue on.
static TLS struct { uint64_t h; uint32_t c, id; } seen[64];
static uint32_t intern(void **ips, int d, int kind) {
  uint64_t h = kind + 1, c = kind + 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < d; i++) h = mix(h ^ (uint64_t)ips[i]), c = mix(c + (uint64_t)ips[i]);
#ifdef HT_HASH_MASK  // tests only: force collisions
  h &= HT_HASH_MASK;
#endif
  c >>= 32;
  h |= 1;
  __typeof__(seen[0]) *hit = &seen[h >> 1 & 63];
  if (hit->h == h && hit->c == c) return hit->id;
  for (uint64_t h0 = h;; h += 2) {
    tab_t *s = shard(stacks, h);
    lock(&s->lk);
    ent_t *e = get(s, h);
    uint32_t id = e ? (uint32_t)e->v : 0;
    if (id && e->v >> 32 != c) { unlock(&s->lk); continue; }
    if (id) *hit = (__typeof__(*hit)){h0, (uint32_t)c, id};
    if (!id) {  // a new stack; when the row or frame arena is full it counts under id 0
      uint64_t off = 0;
      if ((id = atomic_fetch_add(&nstacks, 1) + 1) >= maxstacks) id = 0;
      else if ((off = atomic_fetch_add(&ips_used, d)) + d > ips_cap) atomic_fetch_sub(&ips_used, d), id = 0;
      if (id) {
        memcpy(ips_arena + off, ips, d * sizeof(void *));
        rows[id].ips_off = off; rows[id].depth = d; rows[id].kind = kind;
        if (tl_parts && (kind == GPU || kind == HOST) && ngids < 4096) gids[atomic_fetch_add(&ngids, 1) % 4096] = id;
        put(s, h, c << 32 | id);
      }
    }
    unlock(&s->lk);
    return id;
  }
}

static void peak_check(int force) {
  uint64_t tot = live_total(CPU) + live_total(GPU) + live_total(HOST) + live_total(MMAP), m = peak_mark;
  if (force ? tot < m : tot <= m + m / 100 + 256) return;
  if (atomic_flag_test_and_set(&peak_lk)) return;
  uint32_t n = nstacks < maxstacks ? nstacks : maxstacks - 1;
  for (uint32_t i = 1; i <= n; i++) rows[i].peak_b = rows[i].live_b;
  // other threads' batched updates: read only on a jump of 64 KiB or more (reading their slots on
  // every small wobble cost 25% of malloc on an 8-thread churn)
  for (int i = 0, ns = force || tot - m >= 65536 ? (nslot < SLOTS ? nslot : SLOTS) : 0; i < ns; i++)
    for (int j = 0; j < 16; j++)
      if (lslot[i].pend[j].id && lslot[i].pend[j].id <= n) rows[lslot[i].pend[j].id].peak_b += lslot[i].pend[j].d[4];
  // a free batched in by another thread can land before the allocation it undoes: never a negative site
  for (uint32_t i = 1; i <= n; i++) if ((int64_t)rows[i].peak_b < 0) rows[i].peak_b = 0;
  peak_mark = tot;
  atomic_flag_clear(&peak_lk);
}

// libunwind keeps thread-local state. Loaded at attach (after the program started), its block is
// allocated on a thread's first unwind by the dynamic loader, with the program's own malloc: inside
// an mmap-family hook that may run under the program's allocator lock (PartitionAlloc maps memory
// holding it), a self-deadlock. So such a hook unwinds only on a thread whose block exists already
// (dlinfo reads it without allocating); a malloc-family hook, outside the allocator, creates it.
static void *unw_h;  // libunwind, when its TLS is dynamic (a module with TLS)
static TLS int unw_ok;
static int unw_ready(void) {
  void *t = 0;
  return unw_ok || !unw_h || (unw_ok = !dlinfo(unw_h, RTLD_DI_TLS_DATA, &t) && t);
}
__attribute__((always_inline)) static inline uint32_t stack_id(int kind) {  // inlined: one less frame to unwind
  // no unwind yet: a stackless row of its own, so the exit scan still reads the block as a mapping
  if (kind == MMAP && !unw_ready()) return intern((void *[1]){0}, 0, kind);
  void *buf[DEPTH + 4], **ips = buf;
  int max = kind == GPU || kind == HOST ? DEPTH : depth;  // device buffers are few and big: the whole stack, to
  int d = unw_backtrace(buf, max + 4);                    // the Python line under torch's C++ even with --py
  while (d && (uintptr_t)*ips - self_lo < self_n) ips++, d--;  // our own frames
  if (d > max) d = max;
  // a stack ends before the first return address it repeats: recursion (an interpreter loop
  // re-entered through C, as CPython does for every import and callback) adds no new site
  for (int i = 1; i < d; i++)
    for (int j = 0; j < i; j++)
      if (ips[j] == ips[i]) { d = i; goto cut; }
cut:;
  if (stream_tag && d < max && ips + d < buf + DEPTH + 4) ips[d++] = (void *)(1ULL << 63 | stream_tag);
  return intern(ips, d, kind);
}

static void track(void *p, size_t n, int kind) {
  if (!p || !ready || guard) return;
  guard = 1;
  int err = errno;  // the allocator's errno contract is the program's, not ours
  uint32_t id = stack_id(kind);
  bump(id, 1, n, 0, 1, n);
  add_live(kind, n);
  tab_t *s = shard(live, (uint64_t)p);
  lock(&s->lk); put(s, (uint64_t)p, (uint64_t)n << SIZE_SHIFT | id); unlock(&s->lk);
  last_alloc = ~(uintptr_t)p;
  if (!(++tick & 255) || n >= 65536) flush_mine(), peak_check(0);
  errno = err;
  guard = 0;
}

static uint64_t untrack(void *p) {  // the block's live-table value, 0 when untracked
  if (!p || !ready || guard) return 0;
  uint64_t v;
  tab_t *s = shard(live, (uint64_t)p);
  lock(&s->lk); int hit = del(s, (uint64_t)p, &v); unlock(&s->lk);
  if (!hit) return 0;
  uint32_t id = v & (REACH - 1);
  int64_t n = v >> SIZE_SHIFT;
  bump(id, 0, 0, ~(uintptr_t)p == last_alloc, -1, -n);
  add_live(rows[id].kind, -n);
  last_alloc = 0;
  return v;
}

// Anonymous mappings made outside the hooked allocators: few, so one locked array; a partial
// munmap trims or splits an entry. They join the live table at exit.
typedef struct { uint64_t a, n; uint32_t id, inh; } map_t;  // inh: mapped before a fork
static map_t *mm, *rs, *dn;  // rs: PROT_NONE reservations, counted once mprotect commits them;
static size_t nmm, nrs, ndn; // dn: committed ranges returned with madvise, counted again at report
                             // time for the pages that are resident (reused by plain writes)
static atomic_flag mm_lk;
static uint64_t pages(size_t n) { return (n + 4095) & ~4095UL; }
static void mm_add(void *p, size_t len, uint32_t id) {  // holds mm_lk
  uint64_t n = pages(len);
  row_t *r = &rows[id];
  r->allocs++; r->bytes += n; r->live_n++; r->live_b += n;
  add_live(MMAP, n);
  if (nmm < (1 << 20)) mm[nmm++] = (map_t){(uint64_t)p, n, id, 0};
}
// forget [p, p+len) in every entry of mm (counted) or rs/dn (not); holds mm_lk
static int cut(map_t *v, size_t *nv, void *p, size_t len) {
  uint64_t a = (uint64_t)p, e = a + pages(len);
  int hit = 0, acct = v == mm;
  for (size_t i = 0; i < *nv; i++) {
    map_t m = v[i];
    uint64_t lo = a > m.a ? a : m.a, hi = e < m.a + m.n ? e : m.a + m.n;
    if (lo >= hi) continue;
    hit = 1;
    if (acct) { rows[m.id].live_b -= hi - lo; add_live(MMAP, -(int64_t)(hi - lo)); }
    if (hi < m.a + m.n && *nv < (1 << 20)) { v[(*nv)++] = (map_t){hi, m.a + m.n - hi, m.id, m.inh}; if (acct) rows[m.id].live_n++; }
    if (lo > m.a) v[i].n = lo - m.a;
    else { v[i--] = v[--*nv]; if (acct) rows[m.id].live_n--; }
  }
  return hit;
}
static int mm_cut(void *p, size_t len) { return cut(mm, &nmm, p, len); }
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

static void *scope = RTLD_NEXT;  // attached: RTLD_DEFAULT (see attach)
static void *next(const char *name) {
  if (!r_dlsym) r_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  return r_dlsym(scope, name);
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
  r_mmap = next("mmap"); r_munmap = next("munmap"); r_mprotect = next("mprotect"); r_madvise = next("madvise"); r_mremap = next("mremap");
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
static void retrack(void *p, uint64_t v, int temp) {  // undo untrack: the block is still live
  if (!v || guard) return;
  uint32_t id = v & (REACH - 1);
  int64_t b = v >> SIZE_SHIFT;
  tab_t *s = shard(live, (uint64_t)p);
  lock(&s->lk); put(s, (uint64_t)p, v); unlock(&s->lk);
  bump(id, 0, 0, -temp, 1, b);
  add_live(rows[id].kind, b);
}
HOOK void *realloc(void *p, size_t n) {
  resolve();
  if (in_boot(p)) { void *q = malloc(n); if (q) memcpy(q, p, n); return q; }
  int temp = ~(uintptr_t)p == last_alloc;
  uint64_t v = untrack(p);  // before the call: once p is freed inside, another thread may get it
  void *q = IN(r_realloc(p, n));
  if (q) track(q, n, CPU);
  else if (n || !p) retrack(p, v, temp);  // failed: p is still live; realloc(p, 0) freed it
  return q;
}
HOOK void *reallocarray(void *p, size_t c, size_t n) {
  size_t t;
  if (__builtin_mul_overflow(c, n, &t)) return errno = ENOMEM, (void *)0;
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
  int anon = (fl & MAP_ANONYMOUS) && prot != PROT_NONE, res = (fl & MAP_ANONYMOUS) && prot == PROT_NONE;
  if (p == MAP_FAILED || !watch() || !(anon || res || (fl & MAP_FIXED))) return p;
  guard = 1;
  uint32_t id = anon ? stack_id(MMAP) : 0;
  lock(&mm_lk);
  if (fl & MAP_FIXED) mm_cut(p, n), cut(rs, &nrs, p, n), cut(dn, &ndn, p, n);  // replaces whatever was mapped there
  if (anon) mm_add(p, n, id);
  if (res && nrs < (1 << 16)) rs[nrs++] = (map_t){(uint64_t)p, pages(n), 0, 0};
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
  if (!e) mm_cut(p, n), cut(rs, &nrs, p, n), cut(dn, &ndn, p, n);
  unlock(&mm_lk);
  return e;
}
// Committing part of a reservation makes it a block at this call; PROT_NONE decommits it; madvise
// DONTNEED/FREE (how arenas return pages but keep the range) moves it to dn.
static void commit(void *p, size_t n, int prot, int ret);
HOOK int mprotect(void *p, size_t n, int prot) { resolve(); int e = r_mprotect(p, n, prot); if (!e) commit(p, n, prot, 0); return e; }
HOOK int madvise(void *p, size_t n, int adv) {
  resolve();
  int e = r_madvise(p, n, adv);
  if (!e && (adv == MADV_DONTNEED || adv == MADV_FREE)) commit(p, n, PROT_NONE, 1);
  return e;
}
// a moved mapping takes the parts of its entries inside [p, p+o) along; holds mm_lk
static void move(map_t *v, size_t *nv, uint64_t p, uint64_t o, uint64_t q, uint64_t n) {
  int acct = v == mm;
  for (size_t i = 0, k = *nv; q != p && i < k && *nv < (1 << 20); i++) {
    uint64_t lo = p > v[i].a ? p : v[i].a, hi = p + o < v[i].a + v[i].n ? p + o : v[i].a + v[i].n;
    if (lo >= hi) continue;
    v[(*nv)++] = (map_t){lo + q - p, hi - lo, v[i].id, v[i].inh};  // cut() below drops the source part
    if (acct) { rows[v[i].id].live_b += hi - lo; rows[v[i].id].live_n++; add_live(MMAP, hi - lo); }
  }
  if (q != p) cut(v, nv, (void *)p, o);
  for (size_t i = 0; v == rs && n > o && i < *nv; i++)
    if (v[i].a + v[i].n == q + o && v[i].a >= q) v[i].n += n - o;  // a grown reservation grows
  if (n < o) cut(v, nv, (void *)(q + n), o - n);     // shrunk
}
static void commit(void *p, size_t n, int prot, int ret) {
  if (!nrs || !watch()) return;
  guard = 1;
  uint32_t id = 0;
  uint64_t a = (uint64_t)p, z = a + pages(n);
  lock(&mm_lk);
  for (size_t i = 0; i < nrs; i++) {
    uint64_t lo = a > rs[i].a ? a : rs[i].a, hi = z < rs[i].a + rs[i].n ? z : rs[i].a + rs[i].n;
    if (lo >= hi) continue;
    for (size_t j = 0; ret && j < nmm && ndn < (1 << 20); j++) {
      uint64_t l = lo > mm[j].a ? lo : mm[j].a, h = hi < mm[j].a + mm[j].n ? hi : mm[j].a + mm[j].n;
      if (l < h) dn[ndn++] = (map_t){l, h - l, mm[j].id, 0};
    }
    if (!ret) cut(dn, &ndn, (void *)lo, hi - lo);
    mm_cut((void *)lo, hi - lo);
    if (prot != PROT_NONE) mm_add((void *)lo, hi - lo, id ? id : (id = stack_id(MMAP)));
  }
  unlock(&mm_lk);
  if (id) peak_check(0);
  guard = 0;
}
HOOK void *mremap(void *p, size_t o, size_t n, int fl, ...) {
  void *to = 0;  // the fifth argument exists only with MREMAP_FIXED
  if (fl & MREMAP_FIXED) { va_list ap; va_start(ap, fl); to = va_arg(ap, void *); va_end(ap); }
  resolve();
  if (!watch()) return r_mremap(p, o, n, fl, to);
  guard = 1;
  uint32_t id = stack_id(MMAP);
  lock(&mm_lk);
  void *q = r_mremap(p, o, n, fl, to);
  int res = 0;
  for (size_t i = 0; i < nrs; i++) res |= rs[i].a < (uint64_t)p + o && rs[i].a + rs[i].n > (uint64_t)p;
  uint64_t a = (uint64_t)p, b = (uint64_t)q;
  if (q != MAP_FAILED && (fl & MREMAP_FIXED)) mm_cut(q, n), cut(rs, &nrs, q, n), cut(dn, &ndn, q, n);  // replaced
  if (q != MAP_FAILED && res) move(rs, &nrs, a, o, b, n), move(mm, &nmm, a, o, b, n), move(dn, &ndn, a, o, b, n);
  else if (q != MAP_FAILED && mm_cut(p, o)) mm_add(q, n, id);  // re-homed at the mremap call, like realloc
  unlock(&mm_lk);
  guard = 0;
  return q;
}

// HIP: the Mojo runtime resolves these through dlsym after dlopen, so dlsym is hooked too.
static void *hip_handle;  // a HIP library dlopen'ed RTLD_LOCAL (ctypes): neither global nor named .so.7
static void *hip(const char *name) {
  void *f = next(name), *h;
  if (!f && hip_handle) f = r_dlsym(hip_handle, name);
  if (!f && (h = dlopen("libamdhip64.so.7", RTLD_NOW | RTLD_NOLOAD))) f = r_dlsym(h, name);
  return f;
}
#define REAL(args, name) static int(*r) args; if (!r) r = hip(name)
static uint64_t dev_used(void) {  // device bytes in use (all its users), ~0 when HIP cannot say
  size_t fr, to;
  REAL((size_t *, size_t *), "hipMemGetInfo");
  return r && !r(&fr, &to) ? to - fr : ~0ULL;
}
// before the program's first GPU allocation: what the device's other users hold. HIP is up by then, so
// no_frag runs before HIP's exit destructors: after them hipMemGetInfo aborts (torch + rocprofiler-sdk)
static void no_frag(void) { tl_parts &= 3; }
#define FRAG_BASE() if ((tl_parts & 4) && frag_base == ~0ULL) frag_base = dev_used(), atexit(no_frag)
HOOK int hipMalloc(void **p, size_t n) { REAL((void **, size_t), "hipMalloc"); FRAG_BASE(); int e = IN(r(p, n)); if (!e) track(*p, n, GPU); return e; }
HOOK int hipMallocAsync(void **p, size_t n, void *s) {
  REAL((void **, size_t, void *), "hipMallocAsync"); FRAG_BASE(); int e = IN(r(p, n, s));
  if (!e) stream_tag = tl_parts & 2 ? (uintptr_t)s | 1 : 0, track(*p, n, GPU), stream_tag = 0;  // | 1: the null stream too
  return e;
}
HOOK int hipHostMalloc(void **p, size_t n, unsigned f) { REAL((void **, size_t, unsigned), "hipHostMalloc"); int e = IN(r(p, n, f)); if (!e) track(*p, n, HOST); return e; }
// untracked first (another thread may get the address once it is freed), restored if the free fails
HOOK int hipFree(void *p) { REAL((void *), "hipFree"); uint64_t v = untrack(p); int e = IN(r(p)); if (e) retrack(p, v, 0); return e; }
HOOK int hipHostFree(void *p) { REAL((void *), "hipHostFree"); uint64_t v = untrack(p); int e = IN(r(p)); if (e) retrack(p, v, 0); return e; }
HOOK int hipFreeAsync(void *p, void *s) { REAL((void *, void *), "hipFreeAsync"); uint64_t v = untrack(p); int e = IN(r(p, s)); if (e) retrack(p, v, 0); return e; }

static int same(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
// dlsym can be asked before this library's ifunc slots are bound (ASan's init, run by the loader):
// no libc string call on this path
static int py_on;
static void py_patch(void);
HOOK void *dlsym(void *h, const char *name) {
  if (py_on && !strncmp(name, "PyInit_", 7)) py_patch();  // an extension just loaded, not yet run
  static const struct { const char *n; void *f; } ours[] = {
      {"hipMalloc", hipMalloc}, {"hipMallocAsync", hipMallocAsync}, {"hipHostMalloc", hipHostMalloc},
      {"hipFree", hipFree},     {"hipHostFree", hipHostFree},       {"hipFreeAsync", hipFreeAsync}};
  if (!r_dlsym) r_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  if (!r_dlsym) return 0;  // asked before libdl answers (ASan's init, run by the loader): not found
  for (size_t i = 0; i < sizeof ours / sizeof *ours; i++)  // ours only where the real one exists
    if (same(name, ours[i].n)) {
      void *real = h == RTLD_DEFAULT || h == RTLD_NEXT ? next(name) : r_dlsym(h, name);
      if (real && h != RTLD_DEFAULT && h != RTLD_NEXT) hip_handle = h;
      return real ? ours[i].f : 0;
    }
  // a tail call keeps the caller's return address, which glibc reads to resolve RTLD_NEXT
  MUSTTAIL return r_dlsym(h, name);
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ULL + t.tv_nsec; }
static void report(int final);
static void py_dump(pid_t pid);
static void py_snap(void);  // --py: holder chains for this snapshot
static char outdir[4096] = ".";
static int is_root, nochild;  // the run's first traced process (its .bin says so); --no-children
static uint64_t snap_ns;  // --snapshot-every
static volatile sig_atomic_t got_sig, py_wait, py_pending;
// the sampler reports; nothing unsafe here. Under --py a signal before the runner is ready waits
// for it (heaptide_py_ready), so the PYTHON section is written too.
static void on_term(int sig) { if (py_wait) py_pending = sig; else got_sig = sig; }
static int done;
static volatile int finished;  // the final report is written
// SEGV/BUS/FPE/ILL/ABRT: the sampler writes the report and kills the process with the signal;
// if it cannot within 10 s (crash in the shim, heap too broken), die as before. Best effort.
static char alt[1 << 16];  // the main thread's signal stack: a stack overflow still gets a report
static uint64_t crash_sp;  // the crashed thread sleeps on its signal stack; its stack is scanned from here
static pid_t crash_tid;
static void on_crash(int sig, siginfo_t *si, void *ctx) {
  (void)si;
  crash_sp = ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RSP]; crash_tid = gettid();
  got_sig = sig;
  struct timespec ms = {0, 1000000};
  for (int t = 0; t < 10000 && !finished; t++) nanosleep(&ms, 0);
  signal(sig, SIG_DFL);
  if (sig == SIGABRT) raise(sig);  // the others fault again on return
}
static pid_t child;  // the fork that writes a snapshot
// Samples live bytes; on SIGTERM/SIGINT/SIGHUP writes the final report and dies of the signal;
// writes a snapshot every --snapshot-every, or when OUT/snapshot appears (heaptide snapshot).
static uint64_t tl_last[4096 + 2];  // each timeline row's last value (a sample adds only changes)
static void tl_add(uint64_t i, uint32_t id, uint64_t v, uint64_t *last) {
  if (v != *last && ntl < MAXSAMPLES) tl[ntl][0] = i << 32 | id, tl[ntl++][1] = v, *last = v;
}
// One timeline sample: every watched site whose live bytes moved, and for frag (once the program
// itself has made a GPU allocation: never initialise HIP for it) what the device holds beyond them.
static void timeline(uint64_t i) {
  uint64_t *last = tl_last;
  uint32_t n = ngids < 4096 ? ngids : 4096;
  for (uint32_t g = 0; g < n; g++) tl_add(i, gids[g], rows[gids[g]].live_b, &last[g]);
  uint64_t gpu = live_total(GPU);
  if (!(tl_parts & 4) || !n) return;
  uint64_t used_now = dev_used();
  if (used_now != ~0ULL) tl_add(i, ~0u, used_now > frag_base + gpu ? used_now - frag_base - gpu : 0, &last[4096]);
  void *pool; uint64_t res = 0, used = 0;
  { REAL((void **, int), "hipDeviceGetDefaultMemPool"); if (!r || r(&pool, 0)) return; }
  { REAL((void *, int, void *), "hipMemPoolGetAttribute"); if (!r || r(pool, 5, &res) || r(pool, 7, &used)) return; }  // reserved, used
  tl_add(i, ~0u - 1, res > used ? res - used : 0, &last[4097]);
}
static void sample(void) {
  uint64_t i = nsamples;  // written, then counted: a reader never sees half a sample
  if (i >= MAXSAMPLES) return;
  samples[i][0] = now_ns(); samples[i][1] = live_total(CPU) + live_total(HOST) + live_total(MMAP); samples[i][2] = live_total(GPU);
  nsamples = i + 1;
  if (tl) timeline(i);
}
static void detach(void);
static void *sampler(void *a) {
  (void)a;
  guard = 1;
  struct timespec d = {every_ms / 1000, (long)(every_ms % 1000) * 1000000};
  char trig[4200], dtrig[4200];
  snprintf(trig, sizeof trig, "%s/snapshot", outdir);
  snprintf(dtrig, sizeof dtrig, "%s/detach", outdir);
  // heaptide snapshot touches OUT/snapshot: each traced process answers every new mtime once (the first
  // to remove the file took a request alone, so the root's report often never came)
  struct stat st;
#define MTIME(old) (!stat(trig, &st) ? st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : (old))
  int64_t seen = MTIME(0);  // a request from before this process started is not its own
  for (uint64_t next = now_ns() + snap_ns; !stop;) {
    sample();
    if (got_sig) { int sig = got_sig; report(1); signal(sig, SIG_DFL); raise(sig); }
    if (!access(dtrig, F_OK)) { detach(); unlink(dtrig); return 0; }  // gone: heaptide detach returns
    if (child > 0 && syscall(SYS_wait4, child, 0, WNOHANG | __WCLONE, 0) == child) child = 0;
    int64_t m = MTIME(seen);
    if (child <= 0 && ((snap_ns && now_ns() >= next) || m != seen)) seen = m, py_snap(), report(0), next = now_ns() + snap_ns;
    nanosleep(&d, 0);
  }
  return 0;
}

// Reachability (LSan port): roots = writable segments of loaded
// objects; flood through CPU blocks; unreached = leaked; leaked blocks reached from other
// leaked blocks = indirect.
static int heap(uint64_t v) { int k = rows[v & (REACH - 1)].kind; return k == CPU || k == MMAP; }
static ent_t **work;
static size_t nwork;
// retention: per site, the first holder that reached one of its blocks: a site id, or a root
// (1<<63 | address of the global word; 1<<62 a thread's stack or registers)
static uint64_t *held, cur;
// A word pointing into a block keeps it alive too (LSan): a binary search of the CPU/mmap blocks
// sorted by start, only for words inside [heap_lo, heap_hi). Every 16th start is copied to `top`
// and every 16th of those to `top2` (L2-sized up to ~40M blocks): a word is searched in top2, then
// in a 16-entry run of top, then in a 16-entry run of starts. Searches are branchless (a
// mispredicted step costs more than a load) and run 16 words at a time: each level's run is
// prefetched for all 16 words before any is read, so the cache misses of a large heap overlap.
enum { RUN = 16, BATCH = 16 };
static ent_t *starts;  // {start, its live-table entry}
static uint64_t *top, *top2, nstarts, ntop, heap_lo, heap_hi;
// glibc's bins (main_arena, in malloc's module's data) point at free chunks' headers, and a header lies
// in the last 16 B of the block before it: while that data is scanned (bins = 1), a word equal to the
// next chunk's header address of a block does not hold it (else a leak hides, layout permitting)
static uint64_t ma_base, ma_lo, ma_hi;
static int bins;
static int binned(uint64_t w, const ent_t *x) {
  uint64_t n = x->v >> SIZE_SHIFT, c = n + 23 < 32 ? 32 : (n + 23) & ~15ULL;  // chunk size
  return bins && w == x->k + c - 16;
}
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static void scan(const char *b, const char *e, uint64_t bit) {
  b = (const char *)(((uintptr_t)b + 7) & ~7UL);
  while (b + 8 <= e) {
    uint64_t w[BATCH];
    const char *wa[BATCH];
    const ent_t *r[BATCH];
    const uint64_t *tr[BATCH];
    int k = 0;
    for (; k < BATCH && b + 8 <= e; b += 8) {
      uint64_t v = *(const uint64_t *)b;
      if (v < heap_lo || v >= heap_hi) continue;
      const uint64_t *t = top2;  // last start <= v (top2[0] = heap_lo <= v)
      for (size_t n = (ntop + RUN - 1) / RUN; n > 1; n -= n / 2) t = t[n / 2] <= v ? t + n / 2 : t;
      t = top + (t - top2) * RUN;
      __builtin_prefetch(t); __builtin_prefetch(t + 8);
      tr[k] = t; wa[k] = b; w[k++] = v;
    }
    for (int j = 0; j < k; j++) {
      const uint64_t *t = tr[j];
      for (size_t n = ntop - (t - top) < RUN ? ntop - (t - top) : RUN; n > 1; n -= n / 2) t = t[n / 2] <= w[j] ? t + n / 2 : t;
      r[j] = starts + (t - top) * RUN;
      for (int l = 0; l < RUN * 16; l += 64) __builtin_prefetch((const char *)r[j] + l);
    }
    for (int j = 0; j < k; j++) {
      const ent_t *q = r[j];
      for (size_t n = nstarts - (q - starts) < RUN ? nstarts - (q - starts) : RUN; n > 1; n -= n / 2) q = q[n / 2].k <= w[j] ? q + n / 2 : q;
      __builtin_prefetch((const void *)q->v);
      r[j] = q;
    }
    for (int j = 0; j < k; j++) {
      ent_t *x = (ent_t *)r[j]->v;
      // GPU/host blocks are never roots-reachable: the HIP runtime's own tables point at all of them
      if (w[j] < x->k + (x->v >> SIZE_SHIFT) && !(x->v & (REACH | INDIRECT)) && heap(x->v) && !binned(w[j], x)) {
        x->v |= bit; work[nwork++] = x;
        if (bit == REACH && !held[x->v & (REACH - 1)]) held[x->v & (REACH - 1)] = cur ? cur : 1ULL << 63 | (uint64_t)wa[j];
      }
    }
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
  uint64_t root = cur;  // the root being scanned (0 a global, 1<<62 a thread): restored for the next word
  while (nwork) { ent_t *x = work[--nwork]; cur = x->v & (REACH - 1); contents(x->k, x->v, bit); }
  cur = root;
}
static uintptr_t ld_lo, ld_n;  // the dynamic linker's code
static uint64_t (*segs)[2];  // writable segments, listed before threads park: a parked thread may
static int nsegs;            // hold the loader lock that dl_iterate_phdr and dladdr take
static int root_cb(struct dl_phdr_info *in, size_t sz, void *self) {
  (void)sz;
  if (in->dlpi_addr == (uintptr_t)self) return 0;
  for (int i = 0; i < in->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &in->dlpi_phdr[i];
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && in->dlpi_addr == getauxval(AT_BASE))
      ld_lo = in->dlpi_addr + ph->p_vaddr, ld_n = ph->p_memsz;
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
      uint64_t b = in->dlpi_addr + ph->p_vaddr;
      if (nsegs < 4096) segs[nsegs][0] = b, segs[nsegs++][1] = b + ph->p_memsz;
      if (in->dlpi_addr == ma_base) ma_lo = ma_lo && ma_lo < b ? ma_lo : b, ma_hi = ma_hi > b + ph->p_memsz ? ma_hi : b + ph->p_memsz;
    }
  }
  return 0;
}

// heaptide attach (gdb dlopens this library into a running program): the program's GOT slots were
// bound before we existed, so each one naming a hooked function is pointed at ours. Our own
// definitions come from our handle (a plain &malloc here would bind to libc's, first in scope).
static const char *hooked[] = {"malloc", "calloc", "free", "realloc", "reallocarray", "posix_memalign", "aligned_alloc",
  "memalign", "valloc", "mmap", "mmap64", "munmap", "mprotect", "madvise", "mremap", "hipMalloc", "hipMallocAsync",
  "hipHostMalloc", "hipFree", "hipHostFree", "hipFreeAsync"};
static void *own[sizeof hooked / sizeof *hooked];
static uint64_t (*pre)[2];  // writable anonymous memory at attach: may hold the only pointer to a new block
static int npre;
enum { MAXGOT = 16384 };
static uint64_t (*got)[2];  // each slot attach wrote and its old value: detach writes it back
static int ngot, undo, attached;
static const char *const *pnames = hooked;  // what patch_cb points at ours: attach's list, or py_patch's
static void **pown = own;
static size_t npatch = sizeof hooked / sizeof *hooked;
static int patch_cb(struct dl_phdr_info *in, size_t sz, void *self) {
  (void)sz;
  const ElfW(Dyn) *d = 0;
  uintptr_t ro = 0, ro_n = 0;  // RELRO: read-only again once its slots are written
  for (int i = 0; i < in->dlpi_phnum; i++) {
    if (in->dlpi_phdr[i].p_type == PT_DYNAMIC) d = (void *)(in->dlpi_addr + in->dlpi_phdr[i].p_vaddr);
    if (in->dlpi_phdr[i].p_type == PT_GNU_RELRO) ro = in->dlpi_addr + in->dlpi_phdr[i].p_vaddr, ro_n = in->dlpi_phdr[i].p_memsz;
  }
  if (!d || in->dlpi_addr == (uintptr_t)self) return 0;
  const ElfW(Sym) *sym = 0; const char *str = 0; const ElfW(Rela) *rel[2] = {0}; size_t n[2] = {0};
  for (; d->d_tag != DT_NULL; d++) {
    uintptr_t v = d->d_un.d_ptr < in->dlpi_addr ? in->dlpi_addr + d->d_un.d_ptr : d->d_un.d_ptr;
    if (d->d_tag == DT_SYMTAB) sym = (void *)v;
    if (d->d_tag == DT_STRTAB) str = (void *)v;
    if (d->d_tag == DT_JMPREL) rel[0] = (void *)v;
    if (d->d_tag == DT_RELA) rel[1] = (void *)v;
    if (d->d_tag == DT_PLTRELSZ) n[0] = d->d_un.d_val / sizeof(ElfW(Rela));
    if (d->d_tag == DT_RELASZ) n[1] = d->d_un.d_val / sizeof(ElfW(Rela));
  }
  for (int k = 0; sym && str && k < 2; k++)
    for (size_t j = 0; rel[k] && j < n[k]; j++) {
      uint32_t t = ELF64_R_TYPE(rel[k][j].r_info);
      if (t != R_X86_64_JUMP_SLOT && t != R_X86_64_GLOB_DAT) continue;
      const char *name = str + sym[ELF64_R_SYM(rel[k][j].r_info)].st_name;
      for (size_t h = 0; h < npatch; h++)
        if (pown[h] && !strcmp(name, pnames[h])) {
          void **slot = (void **)(in->dlpi_addr + rel[k][j].r_offset);
          int g = 0;  // detach: only a slot still ours, in an object still loaded (one dlclosed since is unmapped)
          while (undo && g < ngot && got[g][0] != (uintptr_t)slot) g++;
          if (undo && (g == ngot || *slot != pown[h])) continue;
          mprotect((void *)((uintptr_t)slot & ~4095UL), 4096, PROT_READ | PROT_WRITE);
          if (undo) { *slot = (void *)got[g][1]; continue; }
          if (got && ngot < MAXGOT) got[ngot][0] = (uintptr_t)slot, got[ngot++][1] = (uintptr_t)*slot;
          *slot = pown[h];
        }
    }
  if (ro_n) mprotect((void *)(ro & ~4095UL), (ro + ro_n + 4095) / 4096 * 4096 - (ro & ~4095UL), PROT_READ);
  return 0;
}
static ssize_t slurp(const char *path, char *buf, size_t n);
static void unvm(void *p, size_t n);
static void attach(void) {
  Dl_info me; dladdr((void *)attach, &me);
  attached = 1;
  void *h = dlopen(me.dli_fname, RTLD_NOW | RTLD_NOLOAD);
  for (size_t i = 0; h && i < sizeof hooked / sizeof *hooked; i++) own[i] = r_dlsym(h, hooked[i]);
  // Forward to what the dynamic linker bound, which may be the program's own allocator (Chromium's
  // PartitionAlloc, a jemalloc or tcmalloc build export malloc): RTLD_NEXT from a library loaded
  // RTLD_LOCAL finds libc's, and a block allocated before attach then reached libc's free (abort).
  scope = RTLD_DEFAULT; r_malloc = 0; resolve();
  char *maps = vm(1 << 20);  // only when attached: a static buffer cost every program 2 MB resident
  if (!maps) return;
  slurp("/proc/self/maps", maps, 1 << 20);
  pre = vm(4096 * 16); got = vm(MAXGOT * 16);
  for (char *l = maps; pre && *l && npre < 4096; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
    char *q, *nl = strchr(l, '\n');
    uint64_t a = strtoull(l, &q, 16), e = strtoull(q + 1, &q, 16);
    if (q[1] == 'r' && q[2] == 'w' && (!strchr(q, '/') || strchr(q, '/') > nl) && (!strchr(q, '[') || !strncmp(strchr(q, '['), "[heap]", 6)))
      pre[npre][0] = a, pre[npre++][1] = e;
  }
  unvm(maps, 1 << 20);
  dl_iterate_phdr(patch_cb, me.dli_fbase);
}
// Memory from before attach (and memfd heaps) is a root, but not the blocks tracked inside it: those are scanned only
// when reached, so a lost block still shows (a stale pointer in a freed chunk can hide one: an
// attached report can under-report, never over-report).
static void scan_ranges(uint64_t (*rg)[2], int nr, uint64_t root) {
  cur = root;
  for (int r = 0; r < nr; r++) {
    uint64_t at = rg[r][0], e = rg[r][1];
    size_t lo = 0, hi = nstarts;  // first block starting at or after the range, minus one (it may overlap)
    while (lo < hi) { size_t m = (lo + hi) / 2; if (starts[m].k < at) lo = m + 1; else hi = m; }
    for (size_t i = lo ? lo - 1 : 0; i < nstarts && at < e; i++) {
      ent_t *x = (ent_t *)starts[i].v;
      uint64_t end = x->k + (x->v >> SIZE_SHIFT);
      if (end <= at) continue;
      if (x->k >= e) break;
      if (x->k > at) scan_copy(at, x->k, REACH, 0), flood(REACH);
      at = end;
    }
    if (at < e) scan_copy(at, e, REACH, 0), flood(REACH);
  }
  cur = 0;
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
// The park signal: the highest real-time signal from SIGRTMAX-2 down that the program leaves at
// its default, so a program's own handler is never replaced. Once ours it stays installed (a late
// delivery must not hit the default, terminate); chosen again if the program takes it back.
static int park_sig(void) {
  static int sig;
  struct sigaction o, sa = {.sa_sigaction = park, .sa_flags = SA_SIGINFO | SA_RESTART};
  if (sig && !sigaction(sig, 0, &o) && o.sa_sigaction == park) return sig;
  sig = 0;
  for (int s = SIGRTMAX - 2; s >= SIGRTMIN + 2 && !sig; s--)
    if (!sigaction(s, 0, &o) && o.sa_handler == SIG_DFL) sig = s;
  sigfillset(&sa.sa_mask);
  if (sig) sigaction(sig, &sa, 0);
  return sig;
}
// 1 when every other thread is parked or, for a snapshot, asleep in a system call: a signal would
// end that sleep early (nanosleep, poll, epoll_wait return EINTR), so such a thread is read from
// /proc instead (stack pointer only: its saved registers are not scanned)
static int sent, asleep;  // the last stop_threads: signalled, and asleep in a system call (read from /proc)
static int stop_threads(int snap) {
  int sig = park_sig();
  sent = asleep = 0;
  nthr = 0; parked = 0; released = 0;  // no free signal: threads listed, none parked (as if blocked)
  if (!thr) thr = vm(MAXTHR * sizeof(thr_t));  // kept: a late park signal still reads it
  DIR *d = opendir("/proc/self/task");
  for (struct dirent *e; d && (e = readdir(d)) && nthr < MAXTHR;)
    if (e->d_name[0] != '.' && atoi(e->d_name) != gettid()) thr[nthr++] = (thr_t){.tid = atoi(e->d_name)};
  if (d) closedir(d);
  char path[64], buf[4096];
  for (int i = 0; i < nthr; i++) {
    snprintf(path, sizeof path, "/proc/self/task/%d/syscall", thr[i].tid);
    if (snap && slurp(path, buf, sizeof buf) > 0 && buf[0] != 'r') { asleep++; continue; }  // not "running"
    snprintf(path, sizeof path, "/proc/self/task/%d/status", thr[i].tid);
    char *b = slurp(path, buf, sizeof buf) > 0 ? strstr(buf, "SigBlk:") : 0;
    if (sig && b && !(strtoull(b + 7, 0, 16) >> (sig - 1) & 1) && !syscall(SYS_tgkill, getpid(), thr[i].tid, sig)) sent++;
  }
  struct timespec ms = {0, 1000000};
  for (int t = 0; parked < sent && t < 500; t++) nanosleep(&ms, 0);
  for (int i = 0; i < nthr; i++)
    if (!thr[i].sp) {  // "nr a1..a6 sp pc" while blocked in a syscall; read now, a snapshot child cannot
      snprintf(path, sizeof path, "/proc/self/task/%d/syscall", thr[i].tid);
      char *f = slurp(path, buf, sizeof buf) > 0 ? buf : 0;
      for (int k = 0; f && k < 7; k++) f = strchr(f + 1, ' ');
      thr[i].sp = f ? strtoull(f + 1, 0, 16) : 0;
    }
  return parked + asleep == nthr;
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
  int n = 0;
  slurp("/proc/self/maps", maps, sizeof maps);
  // The exiting thread is not scanned: its returned frames leave stale slots (on Ubuntu 24.04 one
  // held c_reach's lost child and turned LSan's 256 B indirect leak into "reachable").
  uint64_t sp, top;
  for (int i = 0; i < nthr; i++) {
    sp = thr[i].tid == crash_tid ? crash_sp : thr[i].sp;
    if (!sp || !(top = stack_top(sp, maps))) continue;
    n++;
    scan((const char *)thr[i].regs, (const char *)(thr[i].regs + NGREG), REACH);
    scan_copy(sp, top, REACH, 1);
    flood(REACH);
  }
  return n;
}

static void wr(int fd, const void *p, size_t n) {
  for (const char *c = p; n;) {
    ssize_t k = write(fd, c, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return;
    c += k; n -= k;
  }
}
static void unvm(void *p, size_t n) { if (p) syscall(SYS_munmap, p, n); }
// One report: at exit or on a signal (final), or a snapshot of a running process. Threads are
// parked before any table is read and the live table is never written except the REACH/INDIRECT
// marks, which a snapshot restores, so the program resumes unchanged. A snapshot is skipped when
// a thread could not be parked: it could be growing the table being scanned.
static atomic_flag reporting;
static void report(int final) {
  struct timespec ms = {0, 1000000};
  while (atomic_flag_test_and_set(&reporting)) nanosleep(&ms, 0);
  if (final) tl_parts &= 3;  // at exit HIP may be torn down: no device calls
  if (tl) sample();  // the state at this report: a program shorter than one --every still has a point
  if (done) { atomic_flag_clear(&reporting); return; }
  if (final) done = 1, ready = 0, stop = 1;
  if (final && child > 0) syscall(SYS_wait4, child, 0, __WCLONE, 0), child = 0;  // a late snapshot must not overwrite this
  int g = guard; guard = 1;
  nsegs = 0; nstarts = 0; nwork = 0; heap_hi = 0;
  segs = vm(4096 * 16);
  Dl_info me, ma;
  ma_base = dladdr((void *)r_malloc, &ma) ? (uint64_t)ma.dli_fbase : 0; ma_lo = ma_hi = 0;
  dladdr((void *)report, &me);
  dl_iterate_phdr(root_cb, me.dli_fbase);
  int all_parked = stop_threads(!final);
  if (!final && !all_parked) {
    released = 1; unvm(segs, 4096 * 16);
    dprintf(2, "heaptide: snapshot skipped: %d of %d threads parked (%d asleep, %d signalled, park signal %d)\n", (int)parked, nthr, asleep, sent, park_sig());
    guard = g; atomic_flag_clear(&reporting); return;
  }
  // A snapshot scans a copy-on-write fork, so the program pauses only for the fork, not the scan.
  // Raw clone: no atfork handlers or malloc locks (a parked thread may hold one); no exit signal,
  // so the program's own wait() never sees it. On failure (no memory) it scans in place.
  pid_t pid = getpid();
  if (!final && (child = syscall(SYS_clone, 0, 0, 0, 0, 0)) > 0) {
    released = 1; unvm(segs, 4096 * 16);
    guard = g; atomic_flag_clear(&reporting); return;
  }
  if (final || !child) flush_all();  // the snapshot fork flushes its copy; in place, the batches stay with their owners
  peak_check(1);  // growth after the last 1% step; after the flush, so batched rows count
  // every block: live-table entries, plus mmap blocks in a side array (never put in the table)
  size_t nd = 0, nm = nmm, na = 0;
  for (size_t i = 0; i < ndn; i++) nd += dn[i].n / 8192 + 1;  // resident runs per range <= pages/2 + 1
  size_t total = nmm + nd;
  for (int i = 0; i < SHARDS; i++) total += live[i].n;
  ent_t **all = vm((total + 1) * 8), *mmv = vm((nmm + nd + 1) * 16);
  uint8_t *was = vm(total + 1);  // REACH already set (inherited over fork): restored after a snapshot
  if (!all || !mmv || !was) goto nomem;
  for (int i = 0; i < SHARDS; i++)
    for (size_t j = 0; j < live[i].cap && na < total; j++)
      if (live[i].t[j].k) all[na++] = &live[i].t[j];
  for (size_t i = 0; i < nmm && na < total; i++) {
    mmv[i] = (ent_t){mm[i].a, mm[i].n << SIZE_SHIFT | mm[i].id | (mm[i].inh ? REACH : 0)};
    all[na++] = &mmv[i];
  }
  for (size_t i = 0; i < ndn; i++) {
    size_t np = dn[i].n / 4096;
    uint8_t *vec = vm(np + 1);
    for (size_t j = 0, k, ok = vec && !mincore((void *)dn[i].a, dn[i].n, vec); ok && j < np; j = k) {
      for (k = j; k < np && vec[k] & 1; k++) ;
      if (k > j && na < total) mmv[nm] = (ent_t){dn[i].a + j * 4096, (k - j) * 4096 << SIZE_SHIFT | dn[i].id}, all[na++] = &mmv[nm++];
      if (k == j) k++;
    }
    unvm(vec, np + 1);
  }
  uint32_t n = nstacks < maxstacks ? nstacks : maxstacks - 1;
  work = vm((na + 1) * 8); starts = vm((na + 1) * 16); held = vm((n + 1) * 8);
  if (!work || !starts || !held) goto nomem;
  for (size_t i = 0; i < na; i++) {
    ent_t *x = all[i];
    was[i] = !!(x->v & REACH);
    if (!heap(x->v)) continue;
    uint64_t e = x->k + (x->v >> SIZE_SHIFT);
    starts[nstarts++] = (ent_t){x->k, (uint64_t)x}; heap_hi = e > heap_hi ? e : heap_hi;
  }
  qsort(starts, nstarts, 16, cmp64);
  heap_lo = nstarts ? starts[0].k : 0;
  ntop = (nstarts + RUN - 1) / RUN;
  top = vm((ntop + 1) * 8); top2 = vm((ntop / RUN + 1) * 8);
  if (!top || !top2) goto nomem;
  for (size_t i = 0; i < nstarts; i += RUN) top[i / RUN] = starts[i].k;
  for (size_t i = 0; i < ntop; i += RUN) top2[i / RUN] = top[i];
  for (int i = 0; i < nsegs; i++)
    bins = segs[i][0] >= ma_lo && segs[i][0] < ma_hi, scan((char *)segs[i][0], (char *)segs[i][1], REACH), bins = 0, flood(REACH);
  if (npre) scan_ranges(pre, npre, 1ULL << 61);
  // Private writable memfd mappings are anonymous memory with a name: Qt's JavaScript engine keeps its
  // GC heap there (JSGCHeap, JSVMStack), other VMs and JITs do the same. Never tracked as blocks (they
  // are file-backed), so they are roots: else everything a script holds would look lost.
  {  // mapped for this scan only: static buffers stayed resident in every program (+2 MB peak)
    char *mp = vm(1 << 20);
    uint64_t (*mf)[2] = vm(4096 * 16);
    int nmf = 0;
    if (mp && mf) {
      slurp("/proc/self/maps", mp, 1 << 20);
      for (char *l = mp; *l && nmf < 4096; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
        char *q, *nl = strchr(l, '\n');  // the name searched in this line only (strstr ran to the end: quadratic)
        uint64_t a = strtoull(l, &q, 16), e = strtoull(q + 1, &q, 16);
        if (q[1] == 'r' && q[2] == 'w' && q[4] == 'p' && memmem(l, nl ? (size_t)(nl - l) : strlen(l), " /memfd:", 8)) mf[nmf][0] = a, mf[nmf++][1] = e;
      }
      if (nmf) scan_ranges(mf, nmf, 1ULL << 60);
    }
    unvm(mp, 1 << 20); unvm(mf, 4096 * 16);
  }
  // LSan's use_ld_allocations: a block with a linker frame is a root (a thread's DTV is held as
  // dtv + 1 in its TCB); blocks inherited over fork are roots too (see forked)
  for (size_t i = 0; i < na; i++) {
    ent_t *x = all[i];
    row_t *r = &rows[x->v & (REACH - 1)];
    for (int k = 0; !(x->v & REACH) && r->kind == CPU && k < r->depth; k++)
      if (ips_arena[r->ips_off + k] - ld_lo < ld_n) x->v |= REACH;
    if (x->v & REACH) work[nwork++] = x;
  }
  flood(REACH);
  cur = 1ULL << 62; int scanned = scan_threads(); cur = 0;
  for (size_t i = 0; i < na; i++)
    if (!(all[i]->v & (REACH | INDIRECT)) && heap(all[i]->v)) contents(all[i]->k, all[i]->v, INDIRECT), flood(INDIRECT);
  uint64_t *direct = vm((n + 1) * 24), *indirect = direct + n + 1, *blocks = indirect + n + 1, sum[3] = {0};
  if (!direct) goto nomem;
  for (size_t i = 0; i < na; i++) {
    ent_t *x = all[i];
    uint64_t id = x->v & (REACH - 1), b = x->v >> SIZE_SHIFT;
    if (x->v & REACH) continue;
    blocks[id]++;
    if (x->v & INDIRECT) { indirect[id] += b; sum[1] += b; } else { direct[id] += b; sum[0] += b; }
  }
  for (uint32_t i = 1; i <= n; i++) sum[2] += rows[i].temps;
  if (!final && child < 0)  // scanned in place: undo the marks
    for (size_t i = 0; i < na; i++) all[i]->v = (all[i]->v & ~(uint64_t)(REACH | INDIRECT)) | (was[i] ? REACH : 0);
  released = 1;

  if (!final) py_dump(pid);  // --py: the PYTHON section of this snapshot, in place before the .bin
  // one file, written as .tmp and renamed: a reader never sees half a report, nor maps from another snapshot
  char path[4200], tmp[4210];
  snprintf(path, sizeof path, "%s/heaptide.%d.bin", outdir, pid);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    uint64_t hdr[8] = {0x4544495450414548ULL /* "HEAPTIDE" */, (uint64_t)pid, n, ips_used, nsamples, (uint64_t)every_ms,
                       7 | (tl ? 8 : 0) | (attached ? 16 : 0) | 32 | (is_root ? 64 : 0) /* leaked block counts, retention, timeline, then
                       maps text; attached; the command line after the maps; root */, 0};
    wr(fd, hdr, sizeof hdr);
    wr(fd, ips_arena, ips_used * 8);
    wr(fd, rows + 1, n * sizeof(row_t));
    wr(fd, direct + 1, n * 8);
    wr(fd, indirect + 1, n * 8);
    wr(fd, samples, nsamples * 24);
    wr(fd, blocks + 1, n * 8);
    wr(fd, held + 1, n * 8);
    if (tl) { uint64_t c = ntl; wr(fd, &c, 8); wr(fd, tl, c * 16); }
    int in = open("/proc/self/maps", O_RDONLY);
    char buf[1 << 16];
    for (ssize_t k; in >= 0 && ((k = read(in, buf, sizeof buf)) > 0 || (k < 0 && errno == EINTR));)
      if (k > 0) wr(fd, buf, k), hdr[7] += k;
    if (in >= 0) close(in);
    uint64_t cl[513] = {0};  // {length, /proc/self/cmdline up to 4 KiB}
    if ((in = open("/proc/self/cmdline", O_RDONLY)) >= 0) { ssize_t k = read(in, cl + 1, 4096); cl[0] = k > 0 ? k : 0; close(in); }
    wr(fd, cl, 8 + cl[0]);
    pwrite(fd, &hdr[7], 8, 56);
    close(fd);
    rename(tmp, path);
  }
  dprintf(2, "heaptide: %spid=%d direct=%lu indirect=%lu live_cpu=%lu live_gpu=%lu live_host=%lu live_mmap=%lu temps=%lu stacks=%u threads=%d/%d -> %s/heaptide.%d.bin\n",
          final ? "" : "snapshot ", pid, sum[0], sum[1], live_total(CPU), live_total(GPU), live_total(HOST), live_total(MMAP), sum[2], n,
          scanned, nthr, outdir, pid);
  if (!final && !child) _exit(0);  // the snapshot fork
  finished = final;
  unvm(segs, 4096 * 16); unvm(all, (total + 1) * 8); unvm(mmv, (nmm + nd + 1) * 16);
  unvm(was, total + 1); unvm(work, (na + 1) * 8); unvm(starts, (na + 1) * 16); unvm(top, (ntop + 1) * 8); unvm(top2, (ntop / RUN + 1) * 8);
  unvm(direct, (n + 1) * 24); unvm(held, (n + 1) * 8);
  guard = g;
  atomic_flag_clear(&reporting);
  return;
nomem:  // out of memory or address space: say so rather than write a report missing blocks
  released = 1;
  dprintf(2, "heaptide: FAIL report: out of memory (ulimit -v?), nothing written\n");
  if (!final && !child) _exit(1);
  guard = g;
  atomic_flag_clear(&reporting);
}
// heaptide detach (OUT/detach, seen by the sampler): the hooks pass through from here on (the flags a
// final report sets, so no report at exit either), attach's GOT slots get their old values back and
// our signal handlers go. The library stays loaded: code may still hold &free from while attached.
static void drop(void *p, size_t n) { if (p) syscall(SYS_madvise, p, n, MADV_DONTNEED); }
static void detach(void) {
  struct timespec ms = {0, 1000000};
  while (atomic_flag_test_and_set(&reporting)) nanosleep(&ms, 0);
  done = 1, ready = 0, stop = 1;
  atomic_flag_clear(&reporting);
  if (child > 0) syscall(SYS_wait4, child, 0, __WCLONE, 0), child = 0;
  Dl_info me; dladdr((void *)detach, &me);
  undo = 1; dl_iterate_phdr(patch_cb, me.dli_fbase);
  struct sigaction sa;
  for (int s = 1; s < 32; s++)
    if (!sigaction(s, 0, &sa) && (sa.sa_handler == on_term || sa.sa_sigaction == on_crash)) signal(s, SIG_DFL);
  // The tables go back to the kernel but stay mapped (they read as zeros): a thread that was inside a
  // hook when the flags flipped finishes on empty tables rather than faulting.
  for (int i = 0; i < SHARDS * 2; i++) {
    tab_t *t = i < SHARDS ? &live[i] : &stacks[i - SHARDS];
    lock(&t->lk); drop(t->t, t->cap * sizeof(ent_t)); t->n = 0; unlock(&t->lk);
  }
  drop(rows, (size_t)maxstacks * sizeof(row_t)); drop(ips_arena, ips_cap * 8); drop(samples, (size_t)MAXSAMPLES * 24);
  drop(tl, (size_t)MAXSAMPLES * 16); drop(mm, sizeof(map_t) << 20); drop(rs, sizeof(map_t) << 20); drop(dn, sizeof(map_t) << 20);
}
// --py, memray's design aggregated here: Python's PyMem and object allocators are wrapped
// (PyMem_SetAllocator; both run with the GIL held) and each block is charged to a Python site, the
// (code object, instruction offset) of its innermost --py-depth frames, hashed and cached. A new
// site is named "file:line <- file:line" once; its code objects are kept alive, since a freed one's
// address could come back as another's. Nothing is allocated per block, unlike tracemalloc (whose
// own bookkeeping the native hooks then tracked too: 30 s and 3.9 GB on bench/py_bench.py).
typedef struct { void *ctx; void *(*malloc)(void *, size_t); void *(*calloc)(void *, size_t, size_t);
                 void *(*realloc)(void *, void *, size_t); void (*free)(void *, void *); } pyalloc_t;
enum { MAXPY = 1 << 16, PYCACHE = 4096, PYNAMES = 64 << 20 };
typedef struct { int64_t bytes, blocks; uint32_t name, hide; } pyrow_t;
static pyalloc_t py_orig[2], py_mine[2];  // PYMEM_DOMAIN_MEM, PYMEM_DOMAIN_OBJ
static tab_t py_live, py_sites, py_byname;  // block -> size << SIZE_SHIFT | site; site hash -> site; name hash -> site
static pyrow_t *py_rows;
static uint32_t py_n = 1;  // site 0: no Python frame
static char *py_names, py_self[4096];
static size_t py_names_used;
static uint64_t py_cache[PYCACHE][2];  // site hash, site
static void *py_codes[MAXPY];  // static: the leak scan sees them held
static int py_depth = 1, py_ncodes, py_lasti;
static TLS int py_in;  // inside our own Python calls, which allocate
static struct {
  void *(*frame)(void), *(*code)(void *), *(*back)(void *), *(*getattr)(void *, const char *);
  int (*at)(void *), (*line)(void *, int), (*gil)(void);
  void (*ungil)(int);
  const char *(*utf8)(void *);
  void (*incref)(void *), (*decref)(void *);
  void *(*call)(void *);
  int (*pending)(int (*)(void *), void *);
  void (*errclear)(void);
} P;
static uint32_t py_name(uint64_t h, void **c, int *at, int d) {  // two offsets on one line are one site
  if (py_n >= MAXPY) return 0;
  uint32_t s = py_n, o = py_names_used, hide = 0;
  for (int i = 0; i < d; i++) {
    void *f = P.getattr(c[i], "co_filename");
    const char *file = f ? P.utf8(f) : 0;
    if (!file) file = "?";
    if (!i) hide = !strcmp(file, py_self) || strstr(file, "/runpy.py") || !strncmp(file, "<frozen ", 8);
    int n = snprintf(py_names + o, PYNAMES - o, "%s%s:%d", i ? " <- " : "", file, py_lasti ? P.line(c[i], at[i]) : at[i]);
    o = n > 0 && o + n < PYNAMES ? o + n : o;
    if (f) P.decref(f);
    if (py_ncodes < MAXPY) P.incref(c[i]), py_codes[py_ncodes++] = c[i];
  }
  py_names[o] = 0;
  uint64_t hn = 1;
  for (uint32_t i = py_names_used; i < o; i++) hn = mix(hn + (uint8_t)py_names[i]);
  ent_t *x = py_byname.t ? slot(&py_byname, hn) : 0;
  if (x && x->k == hn) s = x->v;
  else py_rows[s] = (pyrow_t){.name = py_names_used, .hide = hide}, py_n++, put(&py_byname, hn, s), py_names_used = o + 1 < PYNAMES ? o + 1 : o;
  put(&py_sites, h, s);
  return s;
}
static uint32_t py_site(void) {
  void *f = P.frame(), *c[8];  // the innermost frame is borrowed, the ones behind it are new references
  int at[8], d = 0;
  uint64_t h = 0;
  while (f && d < py_depth) {
    c[d] = P.code(f); at[d] = P.at(f);
    h = mix(h + (uint64_t)c[d] + ((uint64_t)(uint32_t)at[d] << 40));
    void *b = ++d < py_depth ? P.back(f) : 0;
    if (d > 1) P.decref(f);
    f = b;
  }
  if (!d) return 0;
  h |= 1;
  uint64_t *e = py_cache[h & (PYCACHE - 1)];
  if (e[0] != h) {
    ent_t *x = py_sites.t ? slot(&py_sites, h) : 0;
    e[0] = h; e[1] = x && x->k == h ? x->v : py_name(h, c, at, d);
  }
  for (int i = 0; i < d; i++) P.decref(c[i]);
  return e[1];
}
static void py_track(void *p, size_t n) {
  if (!p || py_in || !ready) return;
  py_in = 1;
  int err = errno;
  uint32_t s = py_site();
  put(&py_live, (uint64_t)p, (uint64_t)n << SIZE_SHIFT | s);
  py_rows[s].bytes += n; py_rows[s].blocks++;
  errno = err;
  py_in = 0;
}
static void py_untrack(void *p) {
  uint64_t v;
  if (p && py_live.t && del(&py_live, (uint64_t)p, &v)) py_rows[v & (REACH - 1)].bytes -= v >> SIZE_SHIFT, py_rows[v & (REACH - 1)].blocks--;
}
static void *py_malloc(void *c, size_t n) { pyalloc_t *o = c; void *p = o->malloc(o->ctx, n); py_track(p, n); return p; }
static void *py_calloc(void *c, size_t k, size_t n) { pyalloc_t *o = c; void *p = o->calloc(o->ctx, k, n); py_track(p, k * n); return p; }
static void *py_realloc(void *c, void *p, size_t n) {
  pyalloc_t *o = c; void *q = o->realloc(o->ctx, p, n);
  if (q) py_untrack(p), py_track(q, n);
  return q;
}
static void py_free(void *c, void *p) { pyalloc_t *o = c; py_untrack(p); o->free(o->ctx, p); }
// Extensions report buffers they allocate themselves (numpy's array data) to tracemalloc through
// this pair, which may be called without the GIL: it is taken, as tracemalloc does.
HOOK int PyTraceMalloc_Track(unsigned int dom, uintptr_t p, size_t n) {
  static int (*real)(unsigned int, uintptr_t, size_t);
  if (!real) real = next("PyTraceMalloc_Track");
  if (py_on) { int g = P.gil(); py_untrack((void *)p); py_track((void *)p, n); P.ungil(g); }
  return real ? real(dom, p, n) : 0;
}
HOOK int PyTraceMalloc_Untrack(unsigned int dom, uintptr_t p) {
  static int (*real)(unsigned int, uintptr_t);
  if (!real) real = next("PyTraceMalloc_Untrack");
  if (py_on) py_untrack((void *)p);
  return real ? real(dom, p) : 0;
}
// The runner calls this with the GIL held (ctypes.PyDLL), before the script runs; self = its own path.
HOOK int heaptide_py_start(const char *self) {
  void (*get)(int, pyalloc_t *) = r_dlsym(RTLD_DEFAULT, "PyMem_GetAllocator"), (*set)(int, pyalloc_t *) = r_dlsym(RTLD_DEFAULT, "PyMem_SetAllocator");
  P.frame = r_dlsym(RTLD_DEFAULT, "PyEval_GetFrame"); P.code = r_dlsym(RTLD_DEFAULT, "PyFrame_GetCode");
  P.back = r_dlsym(RTLD_DEFAULT, "PyFrame_GetBack"); P.getattr = r_dlsym(RTLD_DEFAULT, "PyObject_GetAttrString");
  P.line = r_dlsym(RTLD_DEFAULT, "PyCode_Addr2Line"); P.utf8 = r_dlsym(RTLD_DEFAULT, "PyUnicode_AsUTF8");
  P.incref = r_dlsym(RTLD_DEFAULT, "Py_IncRef"); P.decref = r_dlsym(RTLD_DEFAULT, "Py_DecRef");
  P.gil = r_dlsym(RTLD_DEFAULT, "PyGILState_Ensure"); P.ungil = r_dlsym(RTLD_DEFAULT, "PyGILState_Release");
  P.call = r_dlsym(RTLD_DEFAULT, "PyObject_CallNoArgs"); P.pending = r_dlsym(RTLD_DEFAULT, "Py_AddPendingCall");
  P.errclear = r_dlsym(RTLD_DEFAULT, "PyErr_Clear");  // the three: holder chains in snapshots only, optional
  py_lasti = !!(P.at = r_dlsym(RTLD_DEFAULT, "PyFrame_GetLasti"));  // 3.11+; before, the line itself is the key
  if (!py_lasti) P.at = r_dlsym(RTLD_DEFAULT, "PyFrame_GetLineNumber");
  if (!get || !set || !P.frame || !P.code || !P.back || !P.getattr || !P.line || !P.utf8 || !P.incref || !P.decref || !P.at || !P.gil || !P.ungil) return 1;
  if (!(py_rows = cold(MAXPY * sizeof(pyrow_t))) || !(py_names = cold(PYNAMES))) return 1;
  char *e = getenv("HEAPTIDE_PY_DEPTH");
  py_depth = e && atoi(e) > 0 ? (atoi(e) < 8 ? atoi(e) : 8) : 1;
  snprintf(py_self, sizeof py_self, "%s", self);
  py_rows[0].hide = 1;
  for (int i = 0; i < 2; i++) {
    get(i + 1, &py_orig[i]);
    py_mine[i] = (pyalloc_t){&py_orig[i], py_malloc, py_calloc, py_realloc, py_free};
    set(i + 1, &py_mine[i]);
  }
  py_on = 1;
  return 0;
}
// py.<pid>.txt: the 20 Python sites holding the most, as the report prints them. The runner writes it
// at exit (before finalization frees everything) and on a stop signal; a snapshot's fork writes it too.
static int py_top(uint32_t top[20]) {  // the 20 sites holding the most, largest first
  int nt = 0;
  for (uint32_t s = 1; py_on && s < py_n; s++) {
    if (py_rows[s].hide || py_rows[s].bytes <= 0) continue;
    int i = nt < 20 ? nt++ : 20;
    for (; i > 0 && py_rows[top[i - 1]].bytes < py_rows[s].bytes; i--) if (i < 20) top[i] = top[i - 1];
    if (i < 20) top[i] = s;
  }
  return nt;
}
static void py_dump(pid_t pid) {
  if (!py_on) return;
  uint32_t top[20]; int nt = py_top(top);
  char path[4200], tmp[4210];
  snprintf(path, sizeof path, "%s/py.%d.txt", outdir, pid);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  for (int i = 0; i < nt; i++)
    dprintf(fd, "  %ld B in %ld blocks  %s\n", (long)py_rows[top[i]].bytes, (long)py_rows[top[i]].blocks, py_names + py_rows[top[i]].name);
  close(fd);
  rename(tmp, path);
}
HOOK void heaptide_py_dump(void) { py_dump(getpid()); }
// --py holder chains (py/run.py held): which Python object holds the top sites' memory, in Python names.
// The runner maps objects to sites through these, with the GIL held (the Python hooks run under it too).
HOOK int heaptide_py_top(uint32_t top[20]) { return py_top(top); }
HOOK const char *heaptide_py_name(uint32_t s) { return py_on && s && s < py_n ? py_names + py_rows[s].name : ""; }
HOOK long heaptide_py_site_of(uint64_t a) {  // the Python site of the block starting at a, or -1
  ent_t *x = py_live.t ? slot(&py_live, a) : 0;
  return x && x->k == a ? (long)(x->v & (REACH - 1)) : -1;
}
HOOK int heaptide_gpu_top(uint32_t top[5]) {  // the 5 GPU stacks holding the most now
  int nt = 0;
  for (uint32_t s = 1; s <= nstacks && s < maxstacks; s++) {
    if (rows[s].kind != GPU || !rows[s].live_b) continue;
    int i = nt < 5 ? nt++ : 5;
    for (; i > 0 && rows[top[i - 1]].live_b < rows[s].live_b; i--) if (i < 5) top[i] = top[i - 1];
    if (i < 5) top[i] = s;
  }
  return nt;
}
HOOK long heaptide_gpu_site_of(uint64_t a) {  // the stack id of the GPU block starting at a, or -1
  tab_t *t = shard(live, a);
  lock(&t->lk);
  ent_t *x = t->t ? slot(t, a) : 0;
  long id = x && x->k == a && rows[x->v & (REACH - 1)].kind == GPU ? (long)(x->v & (REACH - 1)) : -1;
  unlock(&t->lk);
  return id;
}
// a snapshot asks the runner's held() for chains: it runs on the main thread at the next bytecode,
// with the GIL, so the chains may trail the snapshot's .bin
static void *py_walker;
static int py_walk(void *a) { (void)a; void *r = P.call(py_walker); if (r) P.decref(r); else P.errclear(); return 0; }
HOOK void heaptide_py_walker(void *f) { py_walker = P.call && P.pending && P.errclear ? f : 0; }
static void py_snap(void) { if (py_walker) P.pending(py_walk, 0); }
// A python with libpython linked into the executable (Debian, Ubuntu) defines PyTraceMalloc_Track
// itself, ahead of any preload: an extension's calls never reach ours. Their GOT slots are pointed at
// ours instead, each time an extension loads (its PyInit_ lookup comes before its code runs).
static void py_patch(void) {
  static const char *const n[] = {"PyTraceMalloc_Track", "PyTraceMalloc_Untrack"};
  static void *o[2];
  Dl_info me;
  dladdr((void *)py_patch, &me);
  if (!o[0]) { void *h = dlopen(me.dli_fname, RTLD_NOW | RTLD_NOLOAD); for (int i = 0; h && i < 2; i++) o[i] = r_dlsym(h, n[i]); }
  pnames = n; pown = o; npatch = 2;
  dl_iterate_phdr(patch_cb, me.dli_fbase);
  pnames = hooked; pown = own; npatch = sizeof hooked / sizeof *hooked;
}
// --py: the Python runner's own SIGTERM/SIGHUP handler writes its section, then calls this.
HOOK void heaptide_final(void) { report(1); }
// --py: the runner's handler is installed; a stop signal held until now is sent again
HOOK int heaptide_py_ready(void) { py_wait = 0; return py_pending; }
static void fin(void) {
  report(1);
  if (py_pending && py_wait) signal(py_pending, SIG_DFL), raise(py_pending);  // Python exited before it was ready
}

static int self_cb(struct dl_phdr_info *in, size_t sz, void *self) {
  (void)sz;
  for (int i = 0; in->dlpi_addr == (uintptr_t)self && i < in->dlpi_phnum; i++)
    if (in->dlpi_phdr[i].p_type == PT_LOAD && (in->dlpi_phdr[i].p_flags & PF_X))
      self_lo = in->dlpi_addr + in->dlpi_phdr[i].p_vaddr, self_n = in->dlpi_phdr[i].p_memsz;
  return 0;
}

// fork() holds every lock across the copy (outer first, as the hooks nest them), so the child
// never inherits a table another thread was halfway through resizing or shifting.
static void fork_lock(void) {
  lock(&mm_lk); lock(&peak_lk);
  for (int i = 0; i < SHARDS; i++) lock(&stacks[i].lk), lock(&live[i].lk);
}
static void fork_unlock(void) {
  for (int i = 0; i < SHARDS; i++) unlock(&live[i].lk), unlock(&stacks[i].lk);
  unlock(&peak_lk); unlock(&mm_lk);
}
// A forked child inherits the parent's blocks: they are the parent's leaks, not the child's, but
// may hold the only pointer to a child block. Mark them reachable roots. Other threads are gone:
// their batched site counts are folded in (else the child's frees drive a site negative) and the
// sampler restarted.
static void forked(void) {
  fork_unlock();
  is_root = 0;
  if (nochild) done = 1, ready = 0;  // --no-children: not traced from here, no report
  if (done) return;  // after the final report or a detach: nothing to restart
  flush_all();
  for (int i = 0; i < SHARDS; i++)
    for (size_t j = 0; j < live[i].cap; j++) live[i].t[j].v |= live[i].t[j].k ? REACH : 0;
  for (size_t i = 0; i < nmm; i++) mm[i].inh = 1;
  nsamples = 0, ntl = 0;  // the child's own samples: the parent's indices would point past them
  memset(tl_last, 0, sizeof tl_last);
  pthread_t t;
  guard = 1;
  if (!pthread_create(&t, 0, sampler, 0)) pthread_detach(t);
  guard = 0;
}

static int listed(const char *l, const char *n) {  // n is an item of the comma list l
  size_t k = strlen(n);
  for (const char *p = l; k && (p = strstr(p, n)); p += k)
    if ((p == l || p[-1] == ',') && (p[k] == ',' || !p[k])) return 1;
  return 0;
}
__attribute__((constructor)) static void init(void) {
  { // one unwind now: libunwind's thread key is created while few exist (an index under 32 needs no
    // glibc key block, so pthread_setspecific never allocates inside a hook); its handle for unw_ready
    void *b[2]; Dl_info u; size_t mod = 0;
    guard = 1; unw_backtrace(b, 2); guard = 0;
    if (dladdr((void *)unw_backtrace, &u) && (unw_h = dlopen(u.dli_fname, RTLD_NOW | RTLD_NOLOAD)) && (dlinfo(unw_h, RTLD_DI_TLS_MODID, &mod) || !mod))
      unw_h = 0;  // no thread-local state: always ready
  }
  resolve();
  // --no-children, --only NAME,...: any other process keeps the preload untraced (hooks pass through,
  // no memory, no file). The root is the first traced process, or an exec'd root (same pid).
  const char *rt = getenv("HEAPTIDE_ROOT"), *only = getenv("HEAPTIDE_ONLY");
  is_root = !rt || atoi(rt) == getpid(); nochild = !!getenv("HEAPTIDE_NOCHILD");
  if (!getenv("HEAPTIDE_ATTACH") && ((nochild && !is_root) || (only && !listed(only, program_invocation_short_name)))) return;
  // Mojo allocates through its bundled tcmalloc, which malloc hooks never see. Its runtime's sanitizer
  // switch routes every Mojo allocation to libc malloc/aligned_alloc/free instead (and no tcmalloc
  // arena is mapped): set before Mojo allocates, so no tcmalloc block is ever handed to free
  void (*mojo_libc)(void) = (void (*)(void))r_dlsym(RTLD_DEFAULT, "KGEN_CompilerRT_SetAsanAllocators");
  if (getenv("HEAPTIDE_ATTACH")) attach();  // never the Mojo switch then: a tcmalloc block would reach libc free
  else if (mojo_libc) mojo_libc();
  // ~9 GiB of address space, touched only as used; under ulimit -v the row and frame arenas
  // shrink to a quarter of the limit (fewer distinct stacks), and if even that fails heaptide is off
  struct rlimit as;
  for (; !getrlimit(RLIMIT_AS, &as) && as.rlim_cur != RLIM_INFINITY && maxstacks > 4096 &&
         maxstacks * sizeof(row_t) + ips_cap * 8 > as.rlim_cur / 4;) maxstacks /= 2, ips_cap /= 2;
  rows = cold((size_t)maxstacks * sizeof(row_t));
  ips_arena = cold(ips_cap * 8);
  samples = cold((size_t)MAXSAMPLES * 24);
  mm = cold((size_t)sizeof(map_t) << 20); rs = cold((size_t)sizeof(map_t) << 20); dn = cold((size_t)sizeof(map_t) << 20);
  if (!rows || !ips_arena || !samples || !mm || !rs || !dn) {
    dprintf(2, "heaptide: FAIL out of address space (ulimit -v?): not recording\n");
    return;
  }
  const char *e = getenv("HEAPTIDE_EVERY");
  if (e && atoi(e) > 0) every_ms = atoi(e);
  if ((e = getenv("HEAPTIDE_DEPTH")) && atoi(e) > 0 && atoi(e) < DEPTH) depth = atoi(e);
  Dl_info me; dladdr((void *)init, &me);
  dl_iterate_phdr(self_cb, me.dli_fbase);
  unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
  if ((e = getenv("HEAPTIDE_OUT"))) snprintf(outdir, sizeof outdir, "%s", e);
  if (is_root) {  // OUT/root names it for heaptide snapshot; its children see the variable
    char v[16], path[4200]; snprintf(v, sizeof v, "%d", getpid()); setenv("HEAPTIDE_ROOT", v, 1);
    snprintf(path, sizeof path, "%s/root", outdir);
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) dprintf(f, "%s\n", v), close(f);
  }
  if ((e = getenv("HEAPTIDE_GPU_TIMELINE")) && (tl_parts = atoi(e) & 7)) tl = cold((size_t)MAXSAMPLES * 16);
  if ((e = getenv("HEAPTIDE_SNAPSHOT_EVERY")) && atof(e) > 0) snap_ns = (uint64_t)(atof(e) * 1e9);
  // report on the usual stop signals, only where the program kept the default (it may set its own later)
  struct sigaction sa = {.sa_handler = on_term, .sa_flags = SA_RESTART}, old;
  for (int sig = SIGHUP; sig <= SIGTERM; sig++)
    if ((sig == SIGHUP || sig == SIGINT || sig == SIGTERM) && !sigaction(sig, 0, &old) && old.sa_handler == SIG_DFL) sigaction(sig, &sa, 0);
  py_wait = !!getenv("HEAPTIDE_PY");
  unsetenv("HEAPTIDE_PY");  // its child processes have no runner to wait for
  sigaltstack(&(stack_t){.ss_sp = alt, .ss_size = sizeof alt}, 0);
  sa = (struct sigaction){.sa_sigaction = on_crash, .sa_flags = SA_ONSTACK | SA_SIGINFO};
  sigfillset(&sa.sa_mask);  // the park signal stays blocked: this thread is not waited for
  for (int i = 0, c[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}; i < 5; i++)
    if (!sigaction(c[i], 0, &old) && old.sa_handler == SIG_DFL) sigaction(c[i], &sa, 0);
  pthread_atfork(fork_lock, fork_unlock, forked);
  atexit(fin);  // registered first, so it runs after the program's own atexit/static destructors
  ready = 1;
  pthread_t t;
  guard = 1;
  if (!pthread_create(&t, 0, sampler, 0)) pthread_detach(t);
  guard = 0;
}
