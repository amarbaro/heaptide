#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# v0.1.5 release tarball on aihq-lab (Skylake i5-6200U, no llvm-symbolizer), as root. Prints ok/FAIL lines, last PASS n/n or FAIL k/n.
set -uo pipefail
cd /root && rm -rf heaptide-v0.1.5-linux-x86_64 sk && tar -xzf heaptide-v0.1.5-linux-x86_64.tar.gz && mkdir sk && tar -xzf src.tgz -C sk
H=/root/heaptide-v0.1.5-linux-x86_64/heaptide; cd sk; n=0; f=()
ok() { n=$((n+1)); echo "ok   $1"; }; bad() { n=$((n+1)); f+=("$1"); echo "FAIL $1: $2"; }
num() { grep -m1 -oE " $2=[0-9]+" "$1" | cut -d= -f2; }
for t in c_attach c_detach c_ownmalloc; do cc -O0 -g -o $t tests/$t.c -ldl; done
cc -O0 -g -rdynamic -o c_ownmalloc tests/c_ownmalloc.c -ldl; cc -O0 -g -rdynamic -o c_ownlock tests/c_ownlock.c -ldl -lpthread
cc -O0 -g -Wl,-z,relro,-z,now -o c_detach tests/c_detach.c -ldl; cc -O0 -g -o demo examples/demo.c
$H run --out o/demo -- ./demo > /dev/null 2> demo.err; d=$(num demo.err direct); i=$(num demo.err indirect)
[[ $d == 12800 && $i == 6400 ]] && ok "demo direct=$d indirect=$i" || bad demo "direct=$d indirect=$i, want 12800/6400"
$H report o/demo > demo.txt 2>&1; grep -q 'modules only' demo.txt && ok "no symbolizer: modules only" || bad nosym "no modules-only notice"
$H run --py --py-depth 2 --out o/py -- python3 tests/py_deep.py > /dev/null 2>&1; grep -q 'py_deep.py:4 <- ' o/py/py.*.txt 2>/dev/null && ok "--py hooks: $(head -1 o/py/py.*.txt | sed 's/^ *//')" || bad py "no py_deep.py:4 chain"
att() { ./$1 ${2:-} > $1.out 2> $1.err & p=$!; for _ in $(seq 100); do [[ -s $1.out ]] && break; sleep 0.05; done; $H attach "$(head -1 $1.out)" --out o/$1 > /dev/null 2>&1; }
att c_attach; rc=0; wait $p || rc=$?; d=$(num c_attach.err direct); [[ $rc == 0 && ${d:-0} -ge 18000 ]] && ok "attach as root: exit 0, direct=$d" || bad attach "rc=$rc direct=$d"
$H report o/c_attach > attach.txt 2>&1; grep -q 'GROWTH  n/a (attached' attach.txt && ok "attached: GROWTH n/a" || bad growth "no GROWTH n/a"
att c_detach; for _ in $(seq 200); do grep -q leaked c_detach.out && break; sleep 0.05; done; rcd=0; $H detach o/c_detach > /dev/null 2>&1 || rcd=$?; rc=0; wait $p || rc=$?
[[ $rcd == 0 && $rc == 0 && $(grep -c '^heaptide: pid=' c_detach.err) == 0 ]] && ok "detach: exit 0, slot back to libc, no exit report" || bad detach "detach rc=$rcd program rc=$rc"
att c_ownmalloc; rc=0; wait $p || rc=$?; [[ $rc == 0 ]] && ok "own malloc attached: survived" || bad ownmalloc "rc=$rc $(head -c 80 c_ownmalloc.err)"
att c_ownlock attach; rc=0; wait $p || rc=$?; [[ $rc == 0 ]] && ok "own lock attached: no self-deadlock" || bad ownlock_attach "rc=$rc"
rc=0; timeout 60 $H run --out o/olr -- ./c_ownlock > /dev/null 2>&1 || rc=$?; [[ $rc == 0 ]] && ok "own lock run: no self-deadlock" || bad ownlock_run "rc=$rc"
s=$($H report o/demo --folded leaked 2>/dev/null | awk '{ t += $NF } END { print t + 0 }'); [[ $s == 19200 ]] && ok "folded leaked = 19200" || bad folded "sum $s, want 19200"
$H flame o/demo -o demo.html 2>/dev/null; [[ -s demo.html ]] && ! grep -q HEAPTIDE_DATA demo.html && ok "flame page written ($(( $(stat -c %s demo.html) / 1024 )) KiB)" || bad flame "no page or data not spliced"
$H diff o/demo o/demo > diff.txt 2>&1; grep -qi 'unnamed\|symbolizer' diff.txt && ok "diff warns without symbolizer" || bad diff "no warning: $(head -c 120 diff.txt)"
(( ${#f[@]} )) && { echo "FAIL ${#f[@]}/$n ${f[*]}"; exit 1; } || echo "PASS $n/$n (aihq-lab $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs))"
