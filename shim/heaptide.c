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
static void *hip(const char *name) {
  void *f = next(name), *h;
  if (!f && (h = dlopen("libamdhip64.so.7", RTLD_NOW | RTLD_NOLOAD))) f = r_dlsym(h, name);
  return f;
}
#define REAL(args, name) static int(*r) args; if (!r) r = hip(name)
HOOK int hipMalloc(void **p, size_t n) { REAL((void **, size_t), "hipMalloc"); int e = IN(r(p, n)); if (!e) track(*p, n, GPU); return e; }
HOOK int hipMallocAsync(void **p, size_t n, void *s) { REAL((void **, size_t, void *), "hipMallocAsync"); int e = IN(r(p, n, s)); if (!e) track(*p, n, GPU); return e; }
HOOK int hipHostMalloc(void **p, size_t n, unsigned f) { REAL((void **, size_t, unsigned), "hipHostMalloc"); int e = IN(r(p, n, f)); if (!e) track(*p, n, HOST); return e; }
// untracked first (another thread may get the address once it is freed), restored if the free fails
HOOK int hipFree(void *p) { REAL((void *), "hipFree"); uint64_t v = untrack(p); int e = IN(r(p)); if (e) retrack(p, v, 0); return e; }
HOOK int hipHostFree(void *p) { REAL((void *), "hipHostFree"); uint64_t v = untrack(p); int e = IN(r(p)); if (e) retrack(p, v, 0); return e; }
HOOK int hipFreeAsync(void *p, void *s) { REAL((void *, void *), "hipFreeAsync"); uint64_t v = untrack(p); int e = IN(r(p, s)); if (e) retrack(p, v, 0); return e; }

HOOK void *dlsym(void *h, const char *name) {
  static const struct { const char *n; void *f; } ours[] = {
      {"hipMalloc", hipMalloc}, {"hipMallocAsync", hipMallocAsync}, {"hipHostMalloc", hipHostMalloc},
      {"hipFree", hipFree},     {"hipHostFree", hipHostFree},       {"hipFreeAsync", hipFreeAsync}};
  if (!r_dlsym) r_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  for (size_t i = 0; i < sizeof ours / sizeof *ours; i++)  // ours only where the real one exists
    if (!strcmp(name, ours[i].n)) return (h == RTLD_DEFAULT || h == RTLD_NEXT ? next(name) : r_dlsym(h, name)) ? ours[i].f : 0;
  // a tail call keeps the caller's return address, which glibc reads to resolve RTLD_NEXT
  MUSTTAIL return r_dlsym(h, name);
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ULL + t.tv_nsec; }
static void report(int final);
static char outdir[4096] = ".";
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
static void *sampler(void *a) {
  (void)a;
  guard = 1;
  struct timespec d = {every_ms / 1000, (long)(every_ms % 1000) * 1000000};
  char trig[4200];
  snprintf(trig, sizeof trig, "%s/snapshot", outdir);
  for (uint64_t next = now_ns() + snap_ns; !stop;) {
    uint64_t i = nsamples;
    if (i < MAXSAMPLES) {
      samples[i][0] = now_ns(); samples[i][1] = live_total(CPU) + live_total(HOST) + live_total(MMAP); samples[i][2] = live_total(GPU);
      nsamples = i + 1;
    }
    if (got_sig) { int sig = got_sig; report(1); signal(sig, SIG_DFL); raise(sig); }
    if (child > 0 && syscall(SYS_wait4, child, 0, WNOHANG | __WCLONE, 0) == child) child = 0;
    if (child <= 0 && ((snap_ns && now_ns() >= next) || !unlink(trig))) report(0), next = now_ns() + snap_ns;
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
// A word pointing into a block keeps it alive too (LSan): a binary search of the CPU/mmap blocks
// sorted by start, only for words inside [heap_lo, heap_hi). Every 16th start is copied to `top`
// and every 16th of those to `top2` (L2-sized up to ~40M blocks): a word is searched in top2, then
// in a 16-entry run of top, then in a 16-entry run of starts. Searches are branchless (a
// mispredicted step costs more than a load) and run 16 words at a time: each level's run is
// prefetched for all 16 words before any is read, so the cache misses of a large heap overlap.
enum { RUN = 16, BATCH = 16 };
static ent_t *starts;  // {start, its live-table entry}
static uint64_t *top, *top2, nstarts, ntop, heap_lo, heap_hi;
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static void scan(const char *b, const char *e, uint64_t bit) {
  b = (const char *)(((uintptr_t)b + 7) & ~7UL);
  while (b + 8 <= e) {
    uint64_t w[BATCH];
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
      tr[k] = t; w[k++] = v;
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
      if (w[j] < x->k + (x->v >> SIZE_SHIFT) && !(x->v & (REACH | INDIRECT)) && heap(x->v)) { x->v |= bit; work[nwork++] = x; }
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
  while (nwork) { ent_t *x = work[--nwork]; contents(x->k, x->v, bit); }
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
static int stop_threads(void) {  // 1 when every other thread is parked
  int sig = SIGRTMAX - 2, sent = 0;
  nthr = 0; parked = 0; released = 0;
  struct sigaction sa = {.sa_sigaction = park, .sa_flags = SA_SIGINFO | SA_RESTART};
  sigfillset(&sa.sa_mask);
  sigaction(sig, &sa, 0);  // never restored: a late delivery must not hit the default (terminate)
  if (!thr) thr = vm(MAXTHR * sizeof(thr_t));  // kept: a late park signal still reads it
  DIR *d = opendir("/proc/self/task");
  for (struct dirent *e; d && (e = readdir(d)) && nthr < MAXTHR;)
    if (e->d_name[0] != '.' && atoi(e->d_name) != gettid()) thr[nthr++] = (thr_t){.tid = atoi(e->d_name)};
  if (d) closedir(d);
  char path[64], buf[4096];
  for (int i = 0; i < nthr; i++) {
    snprintf(path, sizeof path, "/proc/self/task/%d/status", thr[i].tid);
    char *b = slurp(path, buf, sizeof buf) > 0 ? strstr(buf, "SigBlk:") : 0;
    if (b && !(strtoull(b + 7, 0, 16) >> (sig - 1) & 1) && !syscall(SYS_tgkill, getpid(), thr[i].tid, sig)) sent++;
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
  return parked == nthr;
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
  for (const char *c = p; n;) { ssize_t k = write(fd, c, n); if (k <= 0) return; c += k; n -= k; }
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
  if (done) { atomic_flag_clear(&reporting); return; }
  if (final) done = 1, ready = 0, stop = 1;
  if (final && child > 0) syscall(SYS_wait4, child, 0, __WCLONE, 0), child = 0;  // a late snapshot must not overwrite this
  int g = guard; guard = 1;
  nsegs = 0; nstarts = 0; nwork = 0; heap_hi = 0;
  segs = vm(4096 * 16);
  Dl_info me; dladdr((void *)report, &me);
  dl_iterate_phdr(root_cb, me.dli_fbase);
  int all_parked = stop_threads();
  if (!final && !all_parked) {
    released = 1; unvm(segs, 4096 * 16);
    dprintf(2, "heaptide: snapshot skipped: %d of %d threads parked\n", (int)parked, nthr);
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
  work = vm((na + 1) * 8); starts = vm((na + 1) * 16);
  if (!work || !starts) goto nomem;
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
  for (int i = 0; i < nsegs; i++) scan((char *)segs[i][0], (char *)segs[i][1], REACH), flood(REACH);
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
  int scanned = scan_threads();
  for (size_t i = 0; i < na; i++)
    if (!(all[i]->v & (REACH | INDIRECT)) && heap(all[i]->v)) contents(all[i]->k, all[i]->v, INDIRECT), flood(INDIRECT);
  uint32_t n = nstacks < maxstacks ? nstacks : maxstacks - 1;
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

  // maps first, then the .bin by rename: a report reading the folder never sees half a file
  char path[4200], tmp[4210];
  snprintf(path, sizeof path, "%s/heaptide.%d.maps", outdir, pid);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int in = open("/proc/self/maps", O_RDONLY), out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  char buf[1 << 16];
  for (ssize_t k; in >= 0 && out >= 0 && (k = read(in, buf, sizeof buf)) > 0;) wr(out, buf, k);
  if (in >= 0) close(in);
  if (out >= 0) close(out), rename(tmp, path);
  snprintf(path, sizeof path, "%s/heaptide.%d.bin", outdir, pid);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    uint64_t hdr[8] = {0x4544495450414548ULL /* "HEAPTIDE" */, (uint64_t)pid, n, ips_used, nsamples, (uint64_t)every_ms, 1 /* leaked block counts follow the samples */, 0};
    wr(fd, hdr, sizeof hdr);
    wr(fd, ips_arena, ips_used * 8);
    wr(fd, rows + 1, n * sizeof(row_t));
    wr(fd, direct + 1, n * 8);
    wr(fd, indirect + 1, n * 8);
    wr(fd, samples, nsamples * 24);
    wr(fd, blocks + 1, n * 8);
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
  unvm(direct, (n + 1) * 24);
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

// A forked child inherits the parent's blocks: they are the parent's leaks, not the child's, but
// may hold the only pointer to a child block. Mark them reachable roots; other threads are gone,
// so their locks are released and the sampler restarted.
static void forked(void) {
  for (int i = 0; i < SHARDS; i++) {
    atomic_flag_clear(&live[i].lk); atomic_flag_clear(&stacks[i].lk);
    for (size_t j = 0; j < live[i].cap; j++) live[i].t[j].v |= live[i].t[j].k ? REACH : 0;
  }
  for (size_t i = 0; i < nmm; i++) mm[i].inh = 1;
  atomic_flag_clear(&mm_lk); atomic_flag_clear(&peak_lk);
  nsamples = 0;
  pthread_t t;
  guard = 1;
  if (!pthread_create(&t, 0, sampler, 0)) pthread_detach(t);
  guard = 0;
}

__attribute__((constructor)) static void init(void) {
  resolve();
  // ~9 GiB of address space, touched only as used; under ulimit -v the row and frame arenas
  // shrink to a quarter of the limit (fewer distinct stacks), and if even that fails heaptide is off
  struct rlimit as;
  for (; !getrlimit(RLIMIT_AS, &as) && as.rlim_cur != RLIM_INFINITY && maxstacks > 4096 &&
         maxstacks * sizeof(row_t) + ips_cap * 8 > as.rlim_cur / 4;) maxstacks /= 2, ips_cap /= 2;
  rows = vm((size_t)maxstacks * sizeof(row_t));
  ips_arena = vm(ips_cap * 8);
  samples = vm((size_t)MAXSAMPLES * 24);
  mm = vm((size_t)sizeof(map_t) << 20); rs = vm((size_t)sizeof(map_t) << 20); dn = vm((size_t)sizeof(map_t) << 20);
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
  pthread_atfork(0, 0, forked);
  atexit(fin);  // registered first, so it runs after the program's own atexit/static destructors
  ready = 1;
  pthread_t t;
  guard = 1;
  if (!pthread_create(&t, 0, sampler, 0)) pthread_detach(t);
  guard = 0;
}
