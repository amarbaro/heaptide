#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# All heaptide checks; heaptrack and LSan are the oracles. GPU checks need ROCm (hipcc) and mojo;
# they run through gpu-wait when it is installed. GPU_ARCH overrides the target (default: first
# gfx* from rocminfo). --cpu runs the CPU checks only and says so on the last line (for machines
# without a GPU, and CI); HEAPTIDE_NO_GPU=1 skips the GPU checks and counts that as FAIL.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); cd "$here"
cpu=0; [[ ${1:-} == --cpu ]] && cpu=1
w=.work/tests; rm -rf "$w"; mkdir -p "$w"
./build.sh >/dev/null
fails=() n=0
ok() { n=$((n + 1)); echo "ok   $1"; }
bad() { n=$((n + 1)); fails+=("$1"); echo "FAIL $1: $2"; }
expect() { [[ $3 == "$4" ]] && ok "$1 $2=$4" || bad "$1" "$2 got '$3' want '$4' (log $w/$1.*)"; }
run() { local t=$1; shift; rm -rf "$w/$t"; ./heaptide run --out "$w/$t" "$@" 2> "$w/$t.err" >/dev/null || true
        ./build/heaptide_report "$w/$t" --top 5 > "$w/$t.txt" 2>&1 || true; }
# value of " KEY=N" in the shim summary line; empty when missing (expect() then fails loudly)
sum() { grep -m1 -oE " $2=[0-9]+" "$w/$1.err" | cut -d= -f2 || true; }
has() { grep -qF -- "$2" "$w/$1.txt" && ok "$1 shows '$2'" || bad "$1" "report lacks '$2' ($w/$1.txt)"; }
# waitfor SECONDS CMD...: poll instead of sleeping, so a loaded machine only waits longer
waitfor() { local i; for ((i = 0; i < $1 * 20; i++)); do "${@:2}" && return 0; sleep 0.05; done; return 1; }
# caught PATTERN SIG: the newest process matching PATTERN has a handler for SIG (heaptide is ready)
caught() { local p m; p=$(pgrep -nf -- "$1") || return 1; m=$(awk '/^SigCgt/ {print $2}' "/proc/$p/status" 2>/dev/null) || return 1
           (( (16#${m:-0} >> ($2 - 1)) & 1 )); }

for t in c_leak c_reach c_churn c_grow c_flat c_setup c_mmap c_slow c_fork c_interior c_realloc c_peak c_reserve c_signal c_snapshot c_remap c_crash; do cc -O0 -g -o "build/$t" "tests/$t.c"; done

run c_leak -- build/c_leak
expect c_leak direct "$(sum c_leak direct)" 100005
ht=$(timeout 60 heaptrack --record-only -o "$PWD/$w/ht" build/c_leak 2>&1 | grep -oE "leaked allocations:[[:space:]]*[0-9]+" | grep -oE "[0-9]+$" || true)
expect c_leak heaptrack_leaked_allocs "$ht" 101
has c_leak "lose c_leak.c:3"

run c_reach -- build/c_reach
expect c_reach direct "$(sum c_reach direct)" 128
expect c_reach indirect "$(sum c_reach indirect)" 256
clang -O0 -g -fsanitize=leak -o build/c_reach_lsan tests/c_reach.c
lsan=$(./build/c_reach_lsan 2>&1 | grep -oE "^(Direct|Indirect) leak of [0-9]+" | grep -oE "[0-9]+$" | sort -n | tr '\n' ' ' || true)
expect c_reach lsan_direct_indirect "$lsan" "128 256 "

run c_churn -- build/c_churn
expect c_churn temps "$(sum c_churn temps)" 1001
grep -A1 "^TEMPORARIES" "$w/c_churn.txt" | grep -q "1000 .*temp_site" && ok "c_churn temporaries temp_site 1000" || bad c_churn "temp_site not top temporary"
grep -A1 "^PEAK" "$w/c_churn.txt" | grep -q "104857600 .*burst_site" && ok "c_churn peak burst_site" || bad c_churn "burst_site not peak"
grep -A1 "^HOTSPOTS" "$w/c_churn.txt" | grep -q "50000 .*hot_site" && ok "c_churn hotspot hot_site" || bad c_churn "hot_site not hotspot"

run c_grow --every 20 -- build/c_grow
has c_grow "GROWTH  FAIL"
run c_flat --every 20 -- build/c_flat
has c_flat "GROWTH  PASS"
expect c_flat direct "$(sum c_flat direct)" 0
run c_setup --every 20 -- build/c_setup
has c_setup "GROWTH  PASS"
# --growth-limit: a slow leak under the 5% rule fails once an absolute limit is set; a setup that
# ends still passes with the same limit
run c_slow --every 20 -- build/c_slow
has c_slow "GROWTH  PASS"
./build/heaptide_report "$w/c_slow" --growth-limit 1 > "$w/c_slow_lim.txt" 2>&1 || true
has c_slow_lim "GROWTH  FAIL"
./build/heaptide_report "$w/c_setup" --growth-limit 1 > "$w/c_setup_lim.txt" 2>&1 || true
has c_setup_lim "GROWTH  PASS"

run c_mmap -- build/c_mmap
expect c_mmap direct "$(sum c_mmap direct)" 1097728
expect c_mmap live_mmap "$(sum c_mmap live_mmap)" 57344
has c_mmap "20.0 KiB (direct 20480, indirect 0, 2 blocks) mmap  lose c_mmap.c:12"

# a forked child reports its own leak, not the parent's (LSan reports both in the child), and a
# child block linked only from an inherited block is reachable
run c_fork -- build/c_fork
expect c_fork child_direct "$(grep -m1 -oE " direct=[0-9]+" "$w/c_fork.err" | cut -d= -f2)" 32
expect c_fork parent_direct "$(grep -oE " direct=[0-9]+" "$w/c_fork.err" | sed -n 2p | cut -d= -f2)" 64

# a pointer into a block's middle keeps it alive (LSan agrees: direct 32, indirect 64)
run c_interior -- build/c_interior
expect c_interior direct "$(sum c_interior direct)" 32
expect c_interior indirect "$(sum c_interior indirect)" 64
clang -O0 -g -fsanitize=leak -o build/c_interior_lsan tests/c_interior.c
lsan=$(./build/c_interior_lsan 2>&1 | grep -oE "^(Direct|Indirect) leak of [0-9]+" | grep -oE "[0-9]+$" | sort -n | tr '\n' ' ' || true)
expect c_interior lsan_direct_indirect "$lsan" "32 64 "

# a failed realloc keeps the block, with its size and site
run c_realloc -- build/c_realloc
expect c_realloc direct "$(sum c_realloc direct)" 12345
has c_realloc "main c_realloc.c:3"
# one allocation, never flushed from its thread's batch, still has a PEAK site
run c_peak -- build/c_peak
grep -A1 "^PEAK" "$w/c_peak.txt" | grep -q "12345 .*c_peak.c:3" && ok "c_peak peak 12345" || bad c_peak "no PEAK site ($w/c_peak.txt)"
# a PROT_NONE reservation counts once mprotect commits part of it; PROT_NONE again decommits
run c_reserve -- build/c_reserve
expect c_reserve live_mmap "$(sum c_reserve live_mmap)" 2097152
# 1 MiB lost + the one page reused by a plain write after madvise (resident at exit)
expect c_reserve direct "$(sum c_reserve direct)" 1052672
has c_reserve "mmap  commit c_reserve.c:3"
# a reservation moved and grown by mremap keeps tracking commits at its new address
run c_remap -- build/c_remap
expect c_remap live_mmap "$(sum c_remap live_mmap)" 2097152
expect c_remap direct "$(sum c_remap direct)" 1048576
# dlsym(RTLD_NEXT) from the program resolves past the program (to heaptide's malloc), not past
# libheaptide: the hook tail-calls dlsym so glibc sees the caller
cc -O0 -g -o build/c_next tests/c_next.c
run c_next -- build/c_next
expect c_next direct "$(sum c_next direct)" 4096
# a stop signal still writes the report, and the process still dies of it (exit 128+sig); a
# program's own handler is left alone
for sig in TERM INT; do
  rm -rf "$w/c_sig$sig"; rc=0
  # a background job starts with SIGINT ignored: restore the default first, as a terminal would
  python3 -c "import os,signal,sys; signal.signal(signal.SIGINT, signal.SIG_DFL); os.execvp(sys.argv[1], sys.argv[1:])" \
    ./heaptide run --out "$w/c_sig$sig" -- build/c_signal 2> "$w/c_sig$sig.err" & p=$!
  waitfor 20 caught '^build/c_signal$' "$(kill -l $sig)" || true; pkill -$sig -x c_signal || true
  wait $p || rc=$?
  expect c_signal_$sig exit "$rc" $(( 128 + $(kill -l $sig) ))
  expect c_signal_$sig direct "$(sum c_sig$sig direct)" 4096
done
rm -rf "$w/c_sigown"; rc=0
./heaptide run --out "$w/c_sigown" -- build/c_signal own 2> "$w/c_sigown.err" & p=$!
waitfor 20 caught '^build/c_signal own$' 15 || true; pkill -TERM -x c_signal || true
wait $p || rc=$?
expect c_signal_own exit "$rc" 7
# allocator, mapping and report corner cases (an external review's reproducers)
cc -O0 -g -pthread -o build/c_edge tests/c_edge.c
for m in zero zerobig errno dlsym count partial fixed peak; do rc=0; (ulimit -c 0; exec ./heaptide run --out "$w/edge_$m" -- build/c_edge $m > "$w/edge_$m.out" 2> "$w/edge_$m.err") || rc=$?; expect edge_$m exit "$rc" 0; done
expect edge_zero direct "$(sum edge_zero direct)" 0
expect edge_zerobig direct "$(sum edge_zerobig direct)" 0
expect edge_errno errno "$(cat "$w/edge_errno.out")" errno=12
expect edge_dlsym hip "$(cat "$w/edge_dlsym.out")" hip=0
expect edge_count blocks "$(./build/heaptide_report "$w/edge_count" --json 2>/dev/null | grep -o '"blocks":[0-9]*' | head -1)" '"blocks":1'
expect edge_partial live_mmap "$(sum edge_partial live_mmap)" 8192
expect edge_fixed live_mmap "$(sum edge_fixed live_mmap)" 8192
./build/heaptide_report "$w/edge_peak" > "$w/edge_peak.txt" 2>&1 || true
has edge_peak "258048   cpu    worker"
rc=0; (ulimit -v 1048576; exec ./heaptide run --out "$w/edge_aslim" -- build/c_edge count 2> "$w/edge_aslim.err") || rc=$?
expect edge_aslim exit "$rc" 0
expect edge_aslim direct "$(sum edge_aslim direct)" 64
cc -shared -fPIC -DMOCK_LIB -o build/libamdhip64.so tests/hip_mock.c && cc -o build/hip_mock tests/hip_mock.c -Lbuild -lamdhip64 -Wl,-rpath,"$here/build"
./heaptide run --out "$w/hip_mockfree" -- build/hip_mock > /dev/null 2> "$w/hip_mockfree.err" || true
expect hip_mockfree live_gpu "$(sum hip_mockfree live_gpu)" 4096
mkdir -p "$w/sp ace"; cp build/c_edge "$w/sp ace/c_edge"
./heaptide run --out "$w/edge_space" -- "$w/sp ace/c_edge" count 2>/dev/null || true
./build/heaptide_report "$w/edge_space" > "$w/edge_space.txt" 2>&1 || true
has edge_space "c_edge.c:"
# a damaged or truncated .bin and bad flags are tool errors (exit 2), never a crash or a clean pass
python3 - "$w" <<'PY'
import struct, sys, pathlib
for tag, n in (("trunc", 1), ("huge", 2**64 - 1)):
    d = pathlib.Path(sys.argv[1], "rep_" + tag); d.mkdir(exist_ok=True)
    (d / "heaptide.1.bin").write_bytes(struct.pack("<8Q", 0x4544495450414548, 1, n, 0, 0, 100, 0, 0)); (d / "heaptide.1.maps").write_text("")
PY
for t in rep_trunc rep_huge; do rc=0; ./build/heaptide_report "$w/$t" --json >/dev/null 2>&1 || rc=$?; expect $t exit "$rc" 2; done
for f in "--top" "--typo" "--growth-limit -1"; do rc=0; ./build/heaptide_report "$w/edge_count" $f >/dev/null 2>&1 || rc=$?; expect "report_args($f)" exit "$rc" 2; done
m=$(ls "$w"/edge_count/*.maps); sed -i 's|/c_edge$|/c\\_e"dge|' "$m"
{ HEAPTIDE_SYMBOLIZER=/nonexistent ./build/heaptide_report "$w/edge_count" --json 2>/dev/null || true; } | python3 -c 'import json,sys; json.load(sys.stdin)' && ok "json escapes backslash and quote" || bad json "invalid JSON for a path with \\ and \""
# a crash still writes the report and dies of its signal; a program's own handler is left alone
for a in segv abort overflow own; do
  rc=0; rm -rf "$w/c_crash_$a"; (ulimit -c 0; exec ./heaptide run --out "$w/c_crash_$a" -- build/c_crash $a 2> "$w/c_crash_$a.err") || rc=$?
  expect c_crash_$a exit "$rc" "$(case $a in segv|overflow) echo 139 ;; abort) echo 134 ;; own) echo 9 ;; esac)"
  [[ $a == own ]] || expect c_crash_$a direct "$(sum c_crash_$a direct)" 4096
done
# snapshots of a running process: on request and on a timer, then the exit report
rm -rf "$w/c_snap"
./heaptide run --out "$w/c_snap" -- build/c_snapshot 2> "$w/c_snap.err" & p=$!
waitfor 20 caught '^build/c_snapshot$' 15 || true; sleep 0.2  # a few blocks lost first
./heaptide snapshot "$w/c_snap" 2>/dev/null; waitfor 20 grep -q "snapshot pid" "$w/c_snap.err" || true
read -r mid blk < <({ ./build/heaptide_report "$w/c_snap" --json 2>/dev/null || true; } | python3 -c "import json,sys; r=json.load(sys.stdin)[0]; print(r['direct'], r['leaks'][0]['blocks'])" 2>/dev/null || true)
wait $p || true
(( ${mid:-0} > 0 && ${mid:-0} < 40960 )) && ok "c_snapshot mid-run direct $mid" || bad c_snapshot "mid-run report direct '$mid', want 0 < x < 40960"
expect c_snapshot mid_blocks "${blk:-}" "$(( ${mid:-0} / 1024 ))"  # row counters flushed in the snapshot
expect c_snapshot final_direct "$(grep -v snapshot "$w/c_snap.err" | grep -m1 -oE " direct=[0-9]+" | cut -d= -f2)" 40960
# no process can be forked (RLIMIT_NPROC): the snapshot scans in place and undoes its marks
rm -rf "$w/c_snapnf"
./heaptide run --out "$w/c_snapnf" -- build/c_snapshot 2> "$w/c_snapnf.err" & p=$!
waitfor 20 caught '^build/c_snapshot$' 15 || true; prlimit --pid "$(pgrep -nx c_snapshot)" --nproc=1
./heaptide snapshot "$w/c_snapnf" 2>/dev/null; wait $p || true
grep -q "snapshot pid" "$w/c_snapnf.err" && ok "c_snapshot without fork" || bad c_snapnf "no snapshot ($w/c_snapnf.err)"
expect c_snapnf final_direct "$(grep -v snapshot "$w/c_snapnf.err" | grep -m1 -oE " direct=[0-9]+" | cut -d= -f2)" 40960
run c_snapevery --snapshot-every 0.5 -- build/c_snapshot
(( $(grep -c "snapshot pid" "$w/c_snapevery.err") >= 2 )) && ok "c_snapshot --snapshot-every" || bad c_snapshot "no timed snapshots ($w/c_snapevery.err)"
# exit while another thread holds the loader lock: must finish, not deadlock
cc -O0 -g -pthread -o build/c_dlhang tests/c_dlhang.c
rc=0; timeout 30 ./heaptide run --out "$w/c_dlhang" -- build/c_dlhang 2> "$w/c_dlhang.err" || rc=$?
expect c_dlhang exit "$rc" 0
# non-PIE executables keep source lines
cc -O0 -g -no-pie -o build/c_leak_nopie tests/c_leak.c
run c_leak_nopie -- build/c_leak_nopie
has c_leak_nopie "lose c_leak.c:3"
# hash collisions never merge sites: a shim with a 4-bit stack hash reports as many sites as the
# normal one (20 leaking functions)
{ echo "extern void *malloc(unsigned long);"; for i in $(seq 20); do echo "void *f$i(void) { return malloc($i); }"; done
  echo "int main(void) {"; for i in $(seq 20); do echo "  f$i();"; done; echo "  return 0; }"; } > "$w/c20.c"
cc -O0 -g -o build/c20 "$w/c20.c"
cc -O2 -g -fPIC -shared -fvisibility=hidden -DHT_HASH_MASK=0xF -o build/libheaptide_collide.so shim/heaptide.c -lunwind -ldl -lpthread
sites() { { ./build/heaptide_report "$1" --json 2>/dev/null || true; } | python3 -c "import json,sys; print(len(json.load(sys.stdin)[0]['leaks']))" 2>/dev/null || true; }
run c20 -- build/c20
rm -rf "$w/c20x"; mkdir -p "$w/c20x"; HEAPTIDE_OUT="$PWD/$w/c20x" LD_PRELOAD="$PWD/build/libheaptide_collide.so" build/c20 2>/dev/null || true
expect c_collide sites "$(sites "$w/c20x")" "$(sites "$w/c20")"
# leak totals cover every site, not the first 1000
{ for i in $(seq 1001); do echo "void *f$i(void) { extern void *malloc(unsigned long); return malloc(1); }"; done
  echo "int main(void) {"; for i in $(seq 1001); do echo "  f$i();"; done; echo "  return 0; }"; } > "$w/many.c"
cc -O0 -o build/c_many "$w/many.c"
run c_many -- build/c_many
expect c_many report_direct "$(./build/heaptide_report "$w/c_many" --json 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin)[0]['direct'])" || true)" 1001

cc -O2 -g -pthread -o build/c_tls tests/c_tls.c
run c_tls -- build/c_tls
expect c_tls direct "$(sum c_tls direct)" 0
clang -O2 -g -pthread -fsanitize=leak -o build/c_tls_lsan tests/c_tls.c
lsan=$(./build/c_tls_lsan 2>&1 | grep -cE "^(Direct|Indirect) leak" || true)
expect c_tls lsan_leaks "$lsan" 0

# the README example, so it cannot drift from the code
cc -O0 -g -o build/demo examples/demo.c
run demo --every 50 -- build/demo
expect demo direct "$(sum demo direct)" 12800
expect demo indirect "$(sum demo indirect)" 6400
has demo "12.5 KiB (direct 12800, indirect 0, 100 blocks) cpu  handle_request demo.c:9:13"
has demo "6.25 KiB (direct 0, indirect 6400, 100 blocks) cpu  handle_request demo.c:10:13"
# --json is the agent interface: fields present, --supp filters the leak list like the totals
j=$(./build/heaptide_report "$w/demo" --json 2>/dev/null || true)
expect demo json "$(python3 -c 'import json,sys;d=json.loads(sys.argv[1])[0];l=d["leaks"][0];print(d["growth"],d["window_from_s"],l["direct"],l["indirect"],l["blocks"])' "$j" 2>&1)" "PASS 0 12800 0 100"
printf 'demo.c:10\n' > "$w/supp"
j=$(./build/heaptide_report "$w/demo" --json --supp "$w/supp" 2>/dev/null || true)
expect demo json_supp "$(python3 -c 'import json,sys;d=json.loads(sys.argv[1])[0];print(d["indirect"],len(d["leaks"]))' "$j" 2>&1)" "0 1"
# symbolizer lookup: none found -> modules only, never a crash; one on PATH wins over ROCm's
HEAPTIDE_SYMBOLIZER=/nonexistent ./build/heaptide_report "$w/demo" --top 1 > "$w/nosym.txt" 2>&1 || true
has nosym "sites show modules only"
# report automation: JSON on stdout stays parseable without a symbolizer; a bad --fail-on is an
# error; a directory with a quote in its name works
{ HEAPTIDE_SYMBOLIZER=/nonexistent ./build/heaptide_report "$w/demo" --json 2>/dev/null || true; } | python3 -c "import json,sys; json.load(sys.stdin)" \
  && ok "nosym json parses" || bad nosym "stdout is not JSON"
rc=0; ./build/heaptide_report "$w/demo" --fail-on leak > /dev/null 2>&1 || rc=$?
expect failon bad_value_exit "$rc" 2
mkdir -p "$w/it's"; cp "$w"/demo/heaptide.* "$w/it's/"
./build/heaptide_report "$w/it's" > "$w/quote.txt" 2>&1 || true
has quote "handle_request demo.c:9"
has nosym "100 blocks) cpu  [demo]"
real=$(command -v llvm-symbolizer || echo /opt/rocm/llvm/bin/llvm-symbolizer)
mkdir -p "$w/bin"; printf '#!/bin/sh\necho used > "%s/bin/used"\nexec %s "$@"\n' "$PWD/$w" "$real" > "$w/bin/llvm-symbolizer"
chmod +x "$w/bin/llvm-symbolizer"; PATH="$PWD/$w/bin:$PATH" ./build/heaptide_report "$w/demo" --top 1 > /dev/null 2>&1 || true
expect symbolizer from_path "$(cat "$w/bin/used" 2>/dev/null || true)" used

if python3 -c "import numpy" 2>/dev/null; then
  run py_leak --py -- python3 tests/py_leak.py
  has py_leak "py_leak.py:6"
  has py_leak "py_leak.py:7"
  # numpy's native frames; stripped libpython and numpy resolve to source through debuginfod
  # (symbolized or not depends on the machine's debug info, so either form passes)
  grep -qE "PyArray_NewFromDescr|_multiarray_umath" "$w/py_leak.txt" && ok "py_leak shows numpy's frames" || bad py_leak "no numpy frame ($w/py_leak.txt)"
  [[ -z ${DEBUGINFOD_URLS:-} ]] || has py_leak "obmalloc.c"
  run py_deep --py --py-depth 2 -- python3 tests/py_deep.py
  has py_deep "py_deep.py:4 <- "
  has py_deep "py_deep.py:6"
  # --py keeps its PYTHON section on SIGTERM and next to a snapshot
  rm -rf "$w/py_snap" "$w/py_term"
  ./heaptide run --py --out "$w/py_snap" -- python3 tests/py_hold.py 2>/dev/null & p=$!
  waitfor 30 caught 'tests/py_hold.py' 15 || true; ./heaptide snapshot "$w/py_snap" 2>/dev/null
  waitfor 30 bash -c "grep -qs 'py_hold.py:2' $w/py_snap/py.*.txt" && ok "py_snapshot PYTHON written while running" || bad py_snapshot "no PYTHON section after a snapshot ($w/py_snap)"
  rc=0; pkill -TERM -f '^python3 .*tests/py_hold.py' || true; wait $p || rc=$?
  # a SIGTERM while Python is still starting is held until the runner is ready: both sections
  rm -rf "$w/py_early"; ./heaptide run --py --out "$w/py_early" -- python3 tests/py_hold.py 2>/dev/null & p=$!
  for ((i = 0; i < 2000; i++)); do caught 'tests/py_hold.py' 15 && break; sleep 0.005; done
  rc=0; pkill -TERM -f '^python3 .*tests/py_hold.py' || true; wait $p || rc=$?
  expect py_early exit "$rc" 143
  ls "$w"/py_early/py.*.txt >/dev/null 2>&1 && ok "py_early PYTHON section" || bad py_early "no PYTHON section ($w/py_early)"
  # ready = the runner's own handler is installed; a snapshot's PYTHON section proves it (SigCgt
  # cannot: the shim's handler is there before Python starts)
  ./heaptide run --py --out "$w/py_term" -- python3 tests/py_hold.py 2>/dev/null & p=$!
  waitfor 30 caught 'tests/py_hold.py' 15 || true; ./heaptide snapshot "$w/py_term" 2>/dev/null
  waitfor 30 bash -c "grep -qs 'py_hold.py:2' $w/py_term/py.*.txt" || true; rm -f "$w"/py_term/py.*.txt
  rc=0; pkill -TERM -f '^python3 .*tests/py_hold.py' || true; wait $p || rc=$?
  expect py_term exit "$rc" 143
  ./build/heaptide_report "$w/py_term" > "$w/py_term.txt" 2>&1 || true
  has py_term "py_hold.py:2"
  pyb=$(ls -S "$w"/py_leak/heaptide.*.bin | head -1)
  (( $(stat -c %s "$pyb") < 8000000 )) && ok "py_leak file < 8 MB" || bad py_leak "$pyb is $(stat -c %s "$pyb") B, want < 8 MB"
else
  bad py_leak "numpy missing"
fi

if (( cpu )); then
  :
elif [[ ${HEAPTIDE_NO_GPU:-} == 1 ]]; then
  bad gpu "skipped (HEAPTIDE_NO_GPU=1)"
else
  mojo=${MOJO:-$(command -v mojo || ls "$here/.venv/bin/mojo" 2>/dev/null || true)}
  arch=${GPU_ARCH:-$(rocminfo 2>/dev/null | grep -om1 'gfx[0-9a-f]*' || true)}
  queue=(); command -v gpu-wait >/dev/null && queue=(gpu-wait run --priority 90 --vram 1 --timeout 120 --label heaptide-tests --)
  GPU_BARE_ALLOW=1 timeout 120 /opt/rocm/bin/hipcc -g --offload-arch="$arch" -o build/hip_leak tests/hip_leak.hip 2>/dev/null
  GPU_BARE_ALLOW=1 timeout 300 "$mojo" build -g --target-accelerator "$arch" tests/mojo_leak.mojo -o build/mojo_leak 2>/dev/null || true
  "${queue[@]}" bash -c "cd '$here' && ./heaptide run --out $w/hip_leak -- build/hip_leak 2> $w/hip_leak.err; ./heaptide run --gpu --out $w/mojo_leak -- build/mojo_leak 2> $w/mojo_leak.err" >/dev/null 2>&1 || true
  for t in hip_leak mojo_leak; do ./build/heaptide_report "$w/$t" --top 5 > "$w/$t.txt" 2>&1 || true; done
  expect hip_leak live_gpu "$(sum hip_leak live_gpu)" 5242880
  has hip_leak "hip_leak.hip:4"
  expect mojo_leak live_gpu "$(grep -oE "live_gpu=[1-9][0-9]*" "$w/mojo_leak.err" | cut -d= -f2 || true)" 4194304
  has mojo_leak "mojo_leak.mojo:8"
fi

scope=""; (( cpu )) && scope=" (cpu only: GPU checks not run)"
if (( ${#fails[@]} )); then echo "FAIL ${#fails[@]}/$n ${fails[*]}$scope"; exit 1; fi
echo "PASS $n/$n$scope"
