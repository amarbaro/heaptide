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

for t in c_leak c_reach c_churn c_grow c_flat c_setup c_mmap c_slow c_fork c_interior c_realloc c_peak c_reserve c_signal c_snapshot c_remap c_crash c_steady c_forkmt c_rtsig c_cache c_sleep c_diff c_attach c_binptr; do cc -O0 -g -o "build/$t" "tests/$t.c"; done

run c_leak -- build/c_leak
expect c_leak direct "$(sum c_leak direct)" 100005
ht=$(timeout 60 heaptrack --record-only -o "$PWD/$w/ht" build/c_leak 2>&1 | grep -oE "leaked allocations:[[:space:]]*[0-9]+" | grep -oE "[0-9]+$" || true)
expect c_leak heaptrack_leaked_allocs "$ht" 101
has c_leak "lose c_leak.c:3"

run c_binptr -- build/c_binptr  # glibc's bins point into the 40 B block's tail: still lost
expect c_binptr direct "$(sum c_binptr direct)" 40
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
# heaptide's own memory stays flat under a steady program: RSS and VmSize growth from 1 s to 3 s
# under 1 MiB each (normal: <= 128 KiB; a shim leaking 64 B per 1024 mallocs grows ~21 MiB)
g=$(./heaptide run --out "$w/c_steady" -- build/c_steady 2>/dev/null | grep -oE '[0-9-]+ [0-9-]+$' || true)
[[ $g =~ ^-?[0-9]+\ -?[0-9]+$ ]] && (( ${g% *} < 1024 && ${g#* } < 1024 )) && ok "c_steady shim growth $g KiB" || bad c_steady "shim growth '$g' KiB, want both < 1024"
# which process is which: every report object names its command and the root; children drop out on request
# (bash, not sh: Ubuntu's sh is dash, which leaves through _exit and so writes no report)
rm -rf "$w/proc_all" "$w/proc_nc" "$w/proc_only"
./heaptide run --out "$w/proc_all" -- bash -c 'true; ls >/dev/null; true' 2>/dev/null
expect proc_all root "$(./build/heaptide_report "$w/proc_all" --json --fail-on none 2>/dev/null | python3 -c 'import json,sys
print(";".join(x["cmdline"] + ("*" if x["root"] else "") for x in sorted(json.load(sys.stdin), key=lambda x: -x["root"])))' 2>&1)" "bash -c true; ls >/dev/null; true*;ls"
./heaptide run --no-children --out "$w/proc_nc" -- bash -c 'true; ls >/dev/null; true' 2>/dev/null
expect proc_nc files "$(ls "$w/proc_nc" | grep -c '\.bin$')" 1
./heaptide run --only ls,cat --out "$w/proc_only" -- bash -c 'true; ls >/dev/null; true' 2>/dev/null
expect proc_only root "$(./build/heaptide_report "$w/proc_only" --json --fail-on none 2>/dev/null | python3 -c 'import json,sys
print(";".join(x["exe"] + ("*" if x["root"] else "") for x in json.load(sys.stdin)))' 2>&1)" "/usr/bin/ls*"
# per-process RSS floor: under THP "always" each arena's first touch was a 2 MiB page (sleep: +7.2 MB)
sleep 3 & b=$!; HEAPTIDE_OUT="$PWD/$w" LD_PRELOAD="$PWD/build/libheaptide.so" sleep 3 2>/dev/null & h=$!
waitfor 10 grep -qE 'SigCgt:.*[1-9a-f]' "/proc/$h/status" || true  # the shim's handlers: it is initialised
d=$(( $(awk '/^VmRSS/ {print $2}' /proc/$h/status) - $(awk '/^VmRSS/ {print $2}' /proc/$b/status) )); kill $b $h; wait $b $h 2>/dev/null || true
(( d < 2048 )) && ok "rss_floor +$d kB per process" || bad rss_floor "+$d kB over bare sleep, want < 2048"
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
# reachable growth: PEAK names who holds it (no leak); a table from another function changes the chain
run c_cache -- build/c_cache
expect c_cache direct "$(sum c_cache direct)" 0
has c_cache "held by: make_table c_cache.c:7"
has c_cache "<< global g_cache (c_cache)"
run c_cache_alt -- build/c_cache alt
has c_cache_alt "held by: alt_table c_cache.c:8"
# Mojo's own heap: its allocator is routed from the bundled tcmalloc to libc, so a lost block is seen
mj=${MOJO:-$(command -v mojo || ls "$here/.venv/bin/mojo" 2>/dev/null || true)}
"$mj" build -g tests/mojo_heap.mojo -o build/mojo_heap > "$w/mojo_heap.build" 2>&1 || true
run mojo_heap -- build/mojo_heap
expect mojo_heap direct "$(sum mojo_heap direct)" 8000
has mojo_heap "mojo_heap::lose"
expect mojo_heap tcmalloc_arena "$(sum mojo_heap live_mmap)" 0
expect c_cache json_held "$(./build/heaptide_report "$w/c_cache" --json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin)[0]["held"][0]["held_by"])' 2>&1)" "make_table c_cache.c:7:74 << global g_cache (c_cache)"
# an ASan-built program (shared runtime, gcc's default): heaptide sees every block, ASan still runs;
# static ASan (clang's default) cannot be watched and gets a warning instead of a silent empty report
gcc -O0 -g -fsanitize=address -o build/c_leak_asan tests/c_leak.c
run c_leak_asan -- build/c_leak_asan
# the planted rows, not the total: ASan's runtime sometimes leaves a block of its own (CI: +32 B)
has c_leak_asan "(direct 100000, indirect 0, 100 blocks) cpu  lose c_leak.c:3"
has c_leak_asan "(direct 5, indirect 0, 1 blocks) cpu  lose c_leak.c:3"
grep -q "LeakSanitizer: detected memory leaks" "$w/c_leak_asan.err" && ok "c_leak_asan ASan still reports" || bad c_leak_asan "no ASan report ($w/c_leak_asan.err)"
clang -O0 -g -fsanitize=address -o build/c_leak_sasan tests/c_leak.c
./heaptide run --out "$w/c_leak_sasan" -- build/c_leak_sasan 2> "$w/c_leak_sasan.err" >/dev/null || true
grep -q "links ASan statically" "$w/c_leak_sasan.err" && ok "c_leak_sasan static ASan warned" || bad c_leak_sasan "no warning ($w/c_leak_sasan.err)"
# fork while 8 threads churn: children report only their own leak, no site goes negative
# (a child once inherited a peak taken while another thread's batched free led its allocation)
run c_forkmt -- build/c_forkmt
# a child shows its 200000 B leak, never an inherited block (a damaged table would leak those)
expect c_forkmt children_leak "$(grep -c " direct=200000 " "$w/c_forkmt.err" || true)" 200
rm -rf "$w/c_forkmt1"; mkdir -p "$w/c_forkmt1"; fb=("$w"/c_forkmt/heaptide.*.bin); cp "${fb[0]}" "$w/c_forkmt1/"
./build/heaptide_report "$w/c_forkmt1" > "$w/c_forkmt.txt" 2>&1 || true
[[ $(cat "$w/c_forkmt.txt") != *"   18446744"* ]] && ok "c_forkmt no negative site" || bad c_forkmt "negative site ($w/c_forkmt.txt)"

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
# --gpu-timeline on a mock HIP: a site growing in 3 steps, two streams, the runtime's planted 1 MiB
# per allocation (kept after a free); each part alone, all, and off
mkdir -p build/tl && cc -shared -fPIC -DMOCK_LIB -o build/tl/libamdhip64.so tests/hip_tl.c && cc -O0 -g -o build/hip_tl tests/hip_tl.c -Lbuild/tl -lamdhip64 -Wl,-rpath,"$here/build/tl"
run hip_tl --gpu-timeline all --every 50 -- build/hip_tl
has hip_tl "GPU TIMELINE"
has hip_tl "[stream 0x1000]"
has hip_tl "[stream 0x2000]"
has hip_tl "(device: held beyond live blocks)"
tlj=$({ ./build/heaptide_report "$w/hip_tl" --json 2>/dev/null || true; } | python3 -c 'import json,sys
d={x["site"].split(" ")[0]:[p[1] for p in x["points"]] for x in json.load(sys.stdin)[0]["gpu_timeline"]}
print(d["grow"], d["(device:"][-1])' 2>&1)
expect hip_tl grow_steps_frag "$tlj" "[4194304, 8388608, 12582912] 7340032"
rc=0; ./heaptide run --gpu-timeline all --out "$w/hip_tl_exit" -- build/hip_tl 2> "$w/hip_tl_exit.err" || rc=$?
expect hip_tl exit_after_hip_teardown "$rc" 0  # frag at exit once called HIP after its teardown (torch: 134)
# a forked child's timeline points at its own samples (it kept the parent's: a point at 1.8e13 ms)
TL_FORK=1 ./heaptide run --gpu-timeline all --every 50 --out "$w/hip_tl_fork" -- build/hip_tl 2>/dev/null || true
tlf=$({ ./build/heaptide_report "$w/hip_tl_fork" --json 2>/dev/null || true; } | python3 -c 'import json,sys
print(max(q[0] for p in json.load(sys.stdin) for x in p["gpu_timeline"] for q in x["points"]) < 60000)' 2>&1)
expect hip_tl fork_points_in_run "$tlf" True
run hip_tl_short --gpu-timeline all --every 100000 -- build/hip_tl  # no sample in the run: the report's own
has hip_tl_short "GPU TIMELINE"
run hip_tl_sites --gpu-timeline sites --every 50 -- build/hip_tl
grep -qE "stream 0x|device: held" "$w/hip_tl_sites.txt" && bad hip_tl_sites "streams or frag without asking ($w/hip_tl_sites.txt)" || ok "hip_tl_sites sites only"
run hip_tl_off --every 50 -- build/hip_tl
grep -q "GPU TIMELINE" "$w/hip_tl_off.txt" && bad hip_tl_off "timeline without the flag" || ok "hip_tl_off no timeline by default"
rc=0; ./heaptide run --gpu-timeline bogus -- true 2>/dev/null || rc=$?
expect hip_tl bad_part_exit "$rc" 2
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
python3 - "$w"/edge_count/*.bin <<'PY2'  # the maps text ends the .bin; header field 7 is its length
import struct, sys
b = bytearray(open(sys.argv[1], "rb").read()); n = struct.unpack_from("<Q", b, 56)[0]
m = bytes(b[len(b) - n:]).replace(b"/c_edge\n", b'/c\\_e"dge\n')
open(sys.argv[1], "wb").write(b[:56] + struct.pack("<Q", len(m)) + b[64:len(b) - n] + m)
PY2
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
rc=0; ./heaptide snapshot "$w/c_snap" 2>/dev/null || rc=$?; expect c_snapshot snapshot_exit "$rc" 0  # returns once written: read at once
read -r mid blk < <({ ./build/heaptide_report "$w/c_snap" --json 2>/dev/null || true; } | python3 -c "import json,sys; r=json.load(sys.stdin)[0]; print(r['direct'], r['leaks'][0]['blocks'])" 2>/dev/null || true)
wait $p || true
(( ${mid:-0} > 0 && ${mid:-0} < 40960 )) && ok "c_snapshot mid-run direct $mid" || bad c_snapshot "mid-run report direct '$mid', want 0 < x < 40960"
# the program's own SIGRTMAX-2 handler survives a snapshot (threads park on a free RT signal)
rm -rf "$w/c_rtsig"; ./heaptide run --out "$w/c_rtsig" -- build/c_rtsig > "$w/c_rtsig.out" 2> "$w/c_rtsig.err" & p=$!
waitfor 20 grep -qs held "$w/c_rtsig.out" || true; ./heaptide snapshot "$w/c_rtsig" >/dev/null 2>&1
rc=0; wait $p || rc=$?
expect c_rtsig own_handler_rc "$rc" 0
grep -q "snapshot skipped" "$w/c_rtsig.err" && bad c_rtsig "snapshot skipped ($w/c_rtsig.err)" || ok "c_rtsig snapshot not skipped"
# a snapshot leaves a sleeping thread asleep (no park signal) and still names its stack as the holder
rm -rf "$w/c_sleep"; ./heaptide run --out "$w/c_sleep" -- build/c_sleep > "$w/c_sleep.out" 2> "$w/c_sleep.err" & p=$!
waitfor 20 grep -qs held "$w/c_sleep.out" || true; ./heaptide snapshot "$w/c_sleep" >/dev/null 2>&1
waitfor 20 grep -qs "heaptide: snapshot pid" "$w/c_sleep.err" || true
rm -rf "$w/c_sleep_s"; cp -r "$w/c_sleep" "$w/c_sleep_s"
rc=0; wait $p || rc=$?
expect c_sleep slept_through_rc "$rc" 0
./build/heaptide_report "$w/c_sleep_s" > "$w/c_sleep.txt" 2>&1 || true
has c_sleep "held by: a thread's stack"
# every traced process answers a snapshot (the first to take the request file used to answer alone),
# and --label keeps that state, the root's report included, for diff
rm -rf "$w/snap_all"; ./heaptide run --out "$w/snap_all" -- sh -c 'build/c_sleep >/dev/null & exec build/c_snapshot' 2>/dev/null & p=$!
waitfor 20 test -s "$w/snap_all/root" || true; sleep 0.3  # both have started their samplers
./heaptide snapshot "$w/snap_all" --label s1 2>/dev/null || true
waitfor 5 test "$(find "$w/snap_all" -maxdepth 1 -name 'heaptide.*.bin' -newer "$w/snap_all/snapshot" | wc -l)" -eq 2 && ok "snap_all both processes answered" || bad snap_all "not every process wrote a snapshot ($w/snap_all)"
r=$(cat "$w/snap_all/root" 2>/dev/null || true); [[ -n $r && -e $w/snap_all/s1/heaptide.$r.bin ]] && ok "snap_all label keeps the root's report" || bad snap_all "no root report in $w/snap_all/s1"
wait $p || true
./heaptide diff "$w/snap_all/s1" "$w/snap_all" --fail-on none > /dev/null 2>&1 && ok "snap_all diff label vs exit" || bad snap_all "diff of the label failed"
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
# heaptide flame / --folded: every stack, the same totals as the shim's own summary; the page's tree
# logic (tests/flame_check.mjs) keeps them in every view
fsum() { ./build/heaptide_report "$w/$1" --folded "$2" 2>/dev/null | awk '{ s += $NF } END { print s + 0 }'; }
expect demo folded_leaked "$(fsum demo leaked)" "$(( $(sum demo direct) + $(sum demo indirect) ))"
expect demo folded_live "$(fsum demo live)" "$(( $(sum demo live_cpu) + $(sum demo live_host) + $(sum demo live_mmap) + $(sum demo live_gpu) ))"
./heaptide flame "$w/demo" -o "$w/demo.html" 2>/dev/null || true
grep -q 'HEAPTIDE_DATA' "$w/demo.html" 2>/dev/null && bad flame "data not spliced into $w/demo.html" || ok "flame page written with its data"
./build/heaptide_report "$w/demo" --flame-data > "$w/demo.flame.json" 2>/dev/null || true
node tests/flame_check.mjs report/flame.html "$w/demo.flame.json" handle_request > "$w/demo.flame.txt" 2>&1 && ok "flame views keep totals (demo)" || bad flame "$(grep -m1 FAIL "$w/demo.flame.txt" || echo 'node failed') ($w/demo.flame.txt)"
# --json is the agent interface: fields present, --supp filters the leak list like the totals
j=$(./build/heaptide_report "$w/demo" --json 2>/dev/null || true)
expect demo json "$(python3 -c 'import json,sys;d=json.loads(sys.argv[1])[0];l=d["leaks"][0];print(d["growth"],d["window_from_s"],l["direct"],l["indirect"],l["blocks"])' "$j" 2>&1)" "PASS 0 12800 0 100"
printf 'demo.c:10\n' > "$w/supp"
j=$(./build/heaptide_report "$w/demo" --json --supp "$w/supp" 2>/dev/null || true)
expect demo json_supp "$(python3 -c 'import json,sys;d=json.loads(sys.argv[1])[0];print(d["indirect"],len(d["leaks"]))' "$j" 2>&1)" "0 1"
# heaptide on its own report: no leak outside the Mojo runtime, whose 2 MiB pool appears on large
# dumps (tests/self.supp; checked: that dump fails without it, passes with it). The report's helpers, sort and the
# symbolizer, write their own files: keep only the one whose maps name heaptide_report.
rm -rf "$w/self" "$w/self1"; mkdir -p "$w/self1"
./heaptide run --out "$w/self" -- ./build/heaptide_report "$w/demo" >/dev/null 2>&1 || true
sb=$(grep -l heaptide_report "$w"/self/heaptide.*.bin | head -1 || true); [[ -n $sb ]] && cp "$sb" "$w/self1/"
rc=0; ./build/heaptide_report "$w/self1" --supp tests/self.supp --fail-on leaks > "$w/self.txt" 2>&1 || rc=$?
expect self fail_on_leaks "$rc" 0
has self "exe $here/build/heaptide_report"
# the Mojo runtime's own rows (every frame in libKGENCompilerRTShared or startup) are kept out of LEAKS, PEAK, held
expect self runtime_rows "$(./build/heaptide_report "$w/self1" --json --fail-on none 2>/dev/null | python3 -c 'import json,re,sys
rt = re.compile(r"^(\[libKGENCompilerRTShared\.so\+|KGEN_CompilerRT|operator new|_start|__libc_start|start_thread|clone)")
d = json.load(sys.stdin)[0]; s = [x["site"] for x in d["leaks"] + d["held"]]
print(len(s) > 0, sum("KGEN" in x and all(rt.match(f) for f in x.split(" <- ")) for x in s))' 2>&1)" "True 0"
# debuginfod: a module the server has no file for is noted once, then not asked again for 7 days
strip -o build/c_leak_stripped build/c_leak; rm -rf "$w/dinfo" "$w/dcache"; ./heaptide run --out "$w/dinfo" -- build/c_leak_stripped 2>/dev/null || true
dn() { XDG_CACHE_HOME="$PWD/$w/dcache" DEBUGINFOD_URLS=http://127.0.0.1:9 ./build/heaptide_report "$w/dinfo" --fail-on none 2>&1 >/dev/null | grep -c "no debug file for .*c_leak_stripped" || true; }
expect debuginfod notes_first_second "$(dn) $(dn)" "1 0"
# heaptide diff: a new leak site fails (exit 1), a grown peak is listed, a moved line is the same site
cc -O0 -g -DEXTRA -o build/c_diff_b tests/c_diff.c 2>/dev/null
run c_diff -- build/c_diff
run c_diff_b -- build/c_diff_b
rc=0; ./heaptide diff "$w/c_diff" "$w/c_diff_b" > "$w/diff.txt" 2>&1 || rc=$?
expect diff new_exit "$rc" 1
has diff "NEW  "
grep -q "^NEW .*lose_b c_diff.c" "$w/diff.txt" && ok "diff NEW lose_b" || bad diff "no NEW lose_b ($w/diff.txt)"
grep -q "^GROWN .*1.0 MiB -> 4.0 MiB peak .*buffer c_diff.c" "$w/diff.txt" && ok "diff GROWN buffer" || bad diff "no GROWN buffer ($w/diff.txt)"
grep -q "lose_a" "$w/diff.txt" && bad diff "lose_a moved a line and was reported ($w/diff.txt)" || ok "diff moved line is the same site"
rc=0; ./heaptide diff "$w/c_diff" "$w/c_diff" > "$w/diff_self.txt" 2>&1 || rc=$?
expect diff self_exit "$rc $(wc -c < "$w/diff_self.txt")" "0 0"
rc=0; ./heaptide diff "$w/c_diff" "$w/c_diff_b" --fail-on none >/dev/null 2>&1 || rc=$?
expect diff none_exit "$rc" 0
HEAPTIDE_SYMBOLIZER=/nonexistent ./heaptide diff "$w/c_diff" "$w/c_diff_b" 2> "$w/diff_nosym.err" >/dev/null || true
grep -q "warning: unnamed frames" "$w/diff_nosym.err" && ok "diff warns without names" || bad diff "no warning without llvm-symbolizer ($w/diff_nosym.err)"
expect diff json "$(./heaptide diff "$w/c_diff" "$w/c_diff_b" --json 2>/dev/null | python3 -c 'import json,sys;print(sorted(c["group"] for c in json.load(sys.stdin)["changes"]))' 2>&1 || true)" "['GROWN', 'NEW']"
# Qt's JavaScript heap lives in memfd mappings: a script's strings must not show as leaked in a
# snapshot while it holds them (before memfd heaps were roots: 968,572 B direct). Needs Qt 6 qml6.
if command -v qml6 >/dev/null; then  # Qt 6 runner (/usr/bin/qml can be Qt 5)
  rm -rf "$w/qml_hold"  # a snapshot each second: the script holds its strings for 3 s
  QT_QPA_PLATFORM=offscreen ./heaptide run --snapshot-every 1 --out "$w/qml_hold" -- qml6 -a core tests/qml_hold.qml > /dev/null 2> "$w/qml_hold.err" || true
  qd=$(grep -m1 -oE "snapshot pid=[0-9]+ direct=[0-9]+" "$w/qml_hold.err" | grep -oE "[0-9]+$" || true)
  (( ${qd:-99999} < 4096 )) && ok "qml_hold JS heap held, direct $qd B" || bad qml_hold "direct '${qd:-none}' B in a snapshot, want < 4096 ($w/qml_hold.err)"
fi
# heaptide attach: a process started without heaptide; leaks made after attach are reported, blocks
# reachable only through memory from before attach are not; no ptrace permission: exit 2
rm -rf "$w/c_attach"; build/c_attach > "$w/c_attach.pid" 2> "$w/c_attach.err" & p=$!
waitfor 20 test -s "$w/c_attach.pid" || true
rc=0; ./heaptide attach "$(head -1 "$w/c_attach.pid")" --out "$w/c_attach" 2> "$w/c_attach.attach" || rc=$?
expect c_attach attach_exit "$rc" 0
rc=0; wait $p || rc=$?
expect c_attach program_exit "$rc" 0
./build/heaptide_report "$w/c_attach" > "$w/c_attach.txt" 2>&1 || true
has c_attach "cpu  lose_after c_attach.c:12"
has c_attach "held by: memory from before attach"
has c_attach "GROWTH  n/a (attached"
expect c_attach json_growth "$(./build/heaptide_report "$w/c_attach" --json 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["growth"])' || true)" attached
sed -n '/^LEAKS/,/^GROWTH/p' "$w/c_attach.txt" | grep -q "c_attach.c:19" && bad c_attach "table entries reported as leaks ($w/c_attach.txt)" || ok "c_attach no false leak from before attach"
sleep 30 & sp=$!; rc=0; ./heaptide attach $sp --out "$w/c_attach_denied" 2>/dev/null || rc=$?; kill $sp 2>/dev/null || true
expect c_attach denied_exit "$rc" 2
# a full-RELRO target keeps its GOT read-only after attach (heaptide writes its slots, then restores)
cc -O0 -g -Wl,-z,relro,-z,now -o build/c_attach_now tests/c_attach.c
build/c_attach_now > "$w/c_attach_now.pid" 2>/dev/null & p=$!
waitfor 20 test -s "$w/c_attach_now.pid" || true; ap=$(head -1 "$w/c_attach_now.pid")
./heaptide attach "$ap" --out "$w/c_attach_now" 2>/dev/null || true
# gotperm BIN PID: the permissions of the page holding BIN's GOT slot for malloc in process PID
gotperm() { local off r perm path lo hi base="" gp=""  # bash arithmetic, not gawk's strtonum (Ubuntu's awk is mawk)
  off=$(readelf -rW "$1" | awk '/ malloc@/ {print $1; exit}')
  while read -r r perm _ _ _ path; do
    [[ $path == */${1##*/} ]] || continue; lo=$((16#${r%-*})); hi=$((16#${r#*-})); base=${base:-$lo}
    (( base + 16#$off >= lo && base + 16#$off < hi )) && gp=$perm
  done < "/proc/$2/maps" || true; echo "$gp"; }
gp=$(gotperm build/c_attach_now "$ap")
wait $p || true
expect c_attach got_page_after_attach "$gp" "r--p"
# heaptide detach: a last snapshot, then the process runs on untracked. The program sees its malloc
# GOT slot hold libc's again; the slot stays read-only (full RELRO); nothing after detach is
# reported, and there is no report at its exit.
cc -O0 -g -Wl,-z,relro,-z,now -o build/c_detach tests/c_detach.c -ldl
rm -rf "$w/c_detach"; build/c_detach > "$w/c_detach.out" 2> "$w/c_detach.err" & p=$!
waitfor 20 test -s "$w/c_detach.out" || true; dp=$(head -1 "$w/c_detach.out")
./heaptide attach "$dp" --out "$w/c_detach" 2>/dev/null || true
waitfor 20 grep -q leaked "$w/c_detach.out" || true
rc=0; ./heaptide detach "$w/c_detach" 2> "$w/c_detach.detach" || rc=$?
expect c_detach detach_exit "$rc" 0
waitfor 20 grep -q detached "$w/c_detach.out" || true
expect c_detach got_page_after_detach "$(gotperm build/c_detach "$dp")" "r--p"
rc=0; ./heaptide attach "$dp" --out "$w/c_detach_again" 2>/dev/null || rc=$?  # inert copy loaded: refused, not a silent no-op
expect c_detach reattach_exit "$rc" 2
rc=0; wait $p || rc=$?
expect c_detach program_exit "$rc" 0
./build/heaptide_report "$w/c_detach" > "$w/c_detach.txt" 2>&1 || true
has c_detach "cpu  lose_before c_detach.c:12"
grep -q lose_after "$w/c_detach.txt" && bad c_detach "a leak after detach reported ($w/c_detach.txt)" || ok "c_detach nothing tracked after detach"
expect c_detach exit_reports "$(grep -c '^heaptide: pid=' "$w/c_detach.err" || true)" 0
# attached to a program that exports its own malloc, the hooks forward to that one (as the dynamic
# linker bound it), not libc's: a block from before attach then reaches the free that matches it
cc -O0 -g -rdynamic -o build/c_ownmalloc tests/c_ownmalloc.c -ldl
rm -rf "$w/c_ownmalloc"; build/c_ownmalloc > "$w/c_ownmalloc.out" 2> "$w/c_ownmalloc.err" & p=$!
waitfor 20 test -s "$w/c_ownmalloc.out" || true
./heaptide attach "$(head -1 "$w/c_ownmalloc.out")" --out "$w/c_ownmalloc" 2>/dev/null || true
rc=0; wait $p || rc=$?
expect c_ownmalloc program_exit "$rc" 0
./build/heaptide_report "$w/c_ownmalloc" > "$w/c_ownmalloc.txt" 2>&1 || true
has c_ownmalloc "cpu  strdup"
# an allocator that maps memory under its own lock (PartitionAlloc does): a thread's first unwind
# in the mmap hook must not allocate (libunwind's TLS block, its key block) through that allocator,
# or the thread sleeps on its own lock (Vivaldi hung minutes after attach). Exit 7 = re-entered.
cc -O0 -g -rdynamic -o build/c_ownlock tests/c_ownlock.c -ldl -lpthread
rm -rf "$w/c_ownlock"; build/c_ownlock attach > "$w/c_ownlock.out" 2> "$w/c_ownlock.err" & p=$!
waitfor 20 test -s "$w/c_ownlock.out" || true
./heaptide attach "$(head -1 "$w/c_ownlock.out")" --out "$w/c_ownlock" 2>/dev/null || true
rc=0; wait $p || rc=$?
expect c_ownlock attach_exit "$rc" 0
rc=0; timeout 60 ./heaptide run --out "$w/c_ownlock_run" -- build/c_ownlock > /dev/null 2> "$w/c_ownlock_run.err" || rc=$?
expect c_ownlock run_exit "$rc" 0
# attached, a thread whose first act is mmap has no stack for it: the exit scan must still copy the
# mapping page by page (a guard page inside it faulted the in-place read: exit 139)
cc -O0 -g -o build/c_mmapguard tests/c_mmapguard.c -lpthread
rm -rf "$w/c_mmapguard"; build/c_mmapguard attach > "$w/c_mmapguard.out" 2> "$w/c_mmapguard.err" & p=$!
waitfor 20 test -s "$w/c_mmapguard.out" || true
./heaptide attach "$(head -1 "$w/c_mmapguard.out")" --out "$w/c_mmapguard" 2>/dev/null || true
rc=0; wait $p || rc=$?
expect c_mmapguard attach_exit "$rc" 0
./build/heaptide_report "$w/c_mmapguard" > "$w/c_mmapguard.txt" 2>&1 || true
has c_mmapguard "held by: global keep"
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
has nosym "100 blocks) cpu  [demo+0x"
# the offset is the frame's place in the module: the symbolizer maps it back to the allocating function
off=$(grep -m1 -oE "100 blocks\) cpu  \[demo\+0x[0-9a-f]+" "$w/nosym.txt" | grep -oE "0x[0-9a-f]+$" || true)
sy=$(command -v llvm-symbolizer || echo /opt/rocm/llvm/bin/llvm-symbolizer)
expect nosym offset_symbolizes "$([[ -n $off && -x $sy ]] && "$sy" --obj=build/demo "$off" | head -1)" handle_request
real=$(command -v llvm-symbolizer || echo /opt/rocm/llvm/bin/llvm-symbolizer)
mkdir -p "$w/bin"; printf '#!/bin/sh\necho used > "%s/bin/used"\nexec %s "$@"\n' "$PWD/$w" "$real" > "$w/bin/llvm-symbolizer"
chmod +x "$w/bin/llvm-symbolizer"; PATH="$PWD/$w/bin:$PATH" ./build/heaptide_report "$w/demo" --top 1 > /dev/null 2>&1 || true
expect symbolizer from_path "$(cat "$w/bin/used" 2>/dev/null || true)" used

if python3 -c "import numpy" 2>/dev/null; then
  run py_leak --py -- python3 tests/py_leak.py
  has py_leak "py_leak.py:6"
  has py_leak "py_leak.py:7"
  # one site per line (two bytecode offsets on line 6 are one row); numpy's array data reaches line 7
  # through PyTraceMalloc_Track (50 x 80 KiB)
  expect py_leak line6_rows "$(sed -n '/^PYTHON (/,/^PYTHON HOLDERS/p' "$w/py_leak.txt" | grep -c 'py_leak.py:6$' || true)" 1
  b7=$(sed -n '/^PYTHON/,$p' "$w/py_leak.txt" | awk '/py_leak.py:7$/ {print $1; exit}')
  (( ${b7:-0} >= 4096000 )) && ok "py_leak numpy data on line 7 ($b7 B)" || bad py_leak "line 7 holds '${b7:-none}' B, want >= 4096000 ($w/py_leak.txt)"
  # numpy's native frames; stripped libpython and numpy resolve to source through debuginfod
  # (symbolized or not depends on the machine's debug info, so either form passes)
  grep -qE "PyArray_NewFromDescr|_multiarray_umath" "$w/py_leak.txt" && ok "py_leak shows numpy's frames" || bad py_leak "no numpy frame ($w/py_leak.txt)"
  [[ -z ${DEBUGINFOD_URLS:-} ]] || has py_leak "obmalloc.c"
  run py_mod --py -- python3 -m tests.py_leak  # python -m: the same planted lines through runpy.run_module
  has py_mod "py_leak.py:6"
  has py_mod "py_leak.py:7"
  # one stack: a native (ctypes malloc) and a GPU (ctypes hipMalloc, library loaded RTLD_LOCAL) site
  # name the Python function that made them (CPython's perf trampoline, -X perf, Python >= 3.12)
  run py_mixed --py -- python3 tests/py_mixed.py
  grep -qE "cpu  .*<- py::leak_in_native py_mixed.py" "$w/py_mixed.txt" && ok "py_mixed native site names py::leak_in_native" || bad py_mixed "no py frame on a cpu site ($w/py_mixed.txt)"
  ./build/heaptide_report "$w/py_mixed" --flame-data > "$w/py_mixed.flame.json" 2>/dev/null || true
  node tests/flame_check.mjs report/flame.html "$w/py_mixed.flame.json" py::leak_in_native > "$w/py_mixed.flame.txt" 2>&1 && ok "flame views keep totals, py:: frames (py_mixed)" || bad flame "$(grep -m1 FAIL "$w/py_mixed.flame.txt" || echo 'node failed') ($w/py_mixed.flame.txt)"
  run py_gpu --py -- python3 tests/py_gpu.py "$here/build/tl/libamdhip64.so"
  expect py_gpu live_gpu "$(sum py_gpu live_gpu)" 1048576
  grep -qE "gpu  .*<- py::load_weights py_gpu.py" "$w/py_gpu.txt" && ok "py_gpu GPU site names py::load_weights" || bad py_gpu "no py frame on a gpu site ($w/py_gpu.txt)"
  # which Python object holds a site's memory, in Python names (the 4 bytearrays of line 4: any index)
  run py_held --py -- python3 tests/py_held.py
  expect py_held chain "$(./build/heaptide_report "$w/py_held" --json --fail-on none 2>/dev/null | python3 -c 'import json,re,sys
c = [h["held_by"] for x in json.load(sys.stdin) for h in x["py_held"] if h["site"].endswith("py_held.py:4")]
print(re.sub(r"data\[\d\]", "data[i]", c[0]) if c else "none")' 2>&1)" "bytearray << Box.data[i] << __main__.keep['a']"
  run py_deep --py --py-depth 2 -- python3 tests/py_deep.py
  has py_deep "py_deep.py:4 <- "
  has py_deep "py_deep.py:6"
  # sys.exit() keeps the script's globals (they were freed: the 5 MB vanished) and its status
  rm -rf "$w/py_exit"; rc=0; ./heaptide run --py --out "$w/py_exit" -- python3 tests/py_exit.py >/dev/null 2>&1 || rc=$?
  expect py_exit status "$rc" 3
  ./build/heaptide_report "$w/py_exit" > "$w/py_exit.txt" 2>&1 || true
  be=$(awk '/tests\/py_exit.py:6$/ {print $1; exit}' "$w/py_exit.txt")
  (( ${be:-0} >= 5000000 )) && ok "py_exit line 6 holds $be B" || bad py_exit "line 6 holds '${be:-none}' B, want >= 5000000 ($w/py_exit.txt)"
  # python -c CODE (the runner took "-c" for a script path: FileNotFoundError)
  rm -rf "$w/py_c"; ./heaptide run --py --out "$w/py_c" -- python3 -c 'keep = [bytearray(100000) for _ in range(5)]' >/dev/null 2>&1 || true
  ./build/heaptide_report "$w/py_c" > "$w/py_c.txt" 2>&1 || true
  bc=$(awk '/<string>:1$/ {print $1; exit}' "$w/py_c.txt")
  (( ${bc:-0} >= 500000 )) && ok "py_c <string>:1 holds $bc B" || bad py_c "<string>:1 holds '${bc:-none}' B, want >= 500000 ($w/py_c.txt)"
  # --py keeps its PYTHON section on SIGTERM and next to a snapshot
  rm -rf "$w/py_snap" "$w/py_term"
  ./heaptide run --py --out "$w/py_snap" -- python3 tests/py_hold.py >"$w/py_snap.out" 2>/dev/null & p=$!
  waitfor 30 grep -qs held "$w/py_snap.out" || true; ./heaptide snapshot "$w/py_snap" 2>/dev/null
  waitfor 30 bash -c "grep -qs 'py_hold.py:2' $w/py_snap/py.*.txt" && ok "py_snapshot PYTHON written while running" || bad py_snapshot "no PYTHON section after a snapshot ($w/py_snap)"
  rc=0; pkill -TERM -f '^python3 .*tests/py_hold.py' || true; wait $p || rc=$?
  # a SIGTERM while Python is still starting is held until the runner is ready: both sections
  rm -rf "$w/py_early"; ./heaptide run --py --out "$w/py_early" -- python3 tests/py_hold.py >/dev/null 2>&1 & p=$!
  for ((i = 0; i < 2000; i++)); do caught 'tests/py_hold.py' 15 && break; sleep 0.005; done
  rc=0; pkill -TERM -f '^python3 .*tests/py_hold.py' || true; wait $p || rc=$?
  expect py_early exit "$rc" 143
  ls "$w"/py_early/py.*.txt >/dev/null 2>&1 && ok "py_early PYTHON section" || bad py_early "no PYTHON section ($w/py_early)"
  # ready = the runner's own handler is installed; a snapshot's PYTHON section proves it (SigCgt
  # cannot: the shim's handler is there before Python starts)
  ./heaptide run --py --out "$w/py_term" -- python3 tests/py_hold.py >"$w/py_term.out" 2>/dev/null & p=$!
  waitfor 30 grep -qs held "$w/py_term.out" || true; ./heaptide snapshot "$w/py_term" 2>/dev/null
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
  "${queue[@]}" bash -c "cd '$here' && ./heaptide run --out $w/hip_leak -- build/hip_leak 2> $w/hip_leak.err; ./heaptide run --gpu --out $w/mojo_leak -- build/mojo_leak 2> $w/mojo_leak.err; ./heaptide run --gpu-timeline all --out $w/hip_leak_tl -- build/hip_leak 2> $w/hip_leak_tl.err" >/dev/null 2>&1 || true
  for t in hip_leak mojo_leak hip_leak_tl; do ./build/heaptide_report "$w/$t" --top 5 > "$w/$t.txt" 2>&1 || true; done
  expect hip_leak live_gpu "$(sum hip_leak live_gpu)" 5242880
  has hip_leak "hip_leak.hip:4"
  has hip_leak_tl "GPU TIMELINE"
  expect mojo_leak live_gpu "$(grep -oE "live_gpu=[1-9][0-9]*" "$w/mojo_leak.err" | cut -d= -f2 || true)" 4194304
  has mojo_leak "mojo_leak.mojo:8"
fi

scope=""; (( cpu )) && scope=" (cpu only: GPU checks not run)"
if (( ${#fails[@]} )); then echo "FAIL ${#fails[@]}/$n ${fails[*]}$scope"; exit 1; fi
echo "PASS $n/$n$scope"
