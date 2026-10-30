#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# ht.sh: the heaptide loop for an agent, one line per answer.
#   ht.sh run DIR [heaptide run flags] -- CMD...   run under a memory cap, write DIR/report.json, summarize
#   ht.sh summary DIR                              per process: totals, growth, top direct leaks, warnings
#   ht.sh prove BEFORE AFTER [SITE]                a fix's receipt: direct bytes per site, before -> after
# HT_MEM caps the traced run (systemd MemoryMax, default half of MemAvailable): a blowup kills the
# job, not the terminal. HEAPTIDE names the wrapper (default: on PATH, else this repo's).
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)  # through a symlinked skill dir too
ht=${HEAPTIDE:-$(command -v heaptide || echo "$here/../../../heaptide")}
[[ -x $ht ]] || { echo "FAIL setup: no heaptide (put it on PATH or set HEAPTIDE=)"; exit 2; }
command -v jq >/dev/null || { echo "FAIL setup: jq not found"; exit 2; }

report() { "$ht" report "$1" --json > "$1/report.json" 2> "$1/report.err" || [[ $? == 1 ]] ||
           { echo "FAIL report: $(tail -1 "$1/report.err")"; exit 2; }; }

summary() {
  local d=$1
  [[ -s $d/report.json ]] || report "$d"
  jq -r '.[] | "pid \(.pid) \(.exe | split("/") | last)\(if .root then " (root)" else "" end): direct \(.direct) B, indirect \(.indirect) B, growth \(.growth)"
      + (if (.growth_window_bytes // 0) > 0 then " (+\(.growth_window_bytes) B over the window)" else "" end)
      + (if .ended_without_report then "\n  WARN ended without a report (killed or _exit): no leak scan, the held sites are what it had" else "" end)
      + (if (.before_attach_bytes // 0) > 0 then "\n  WARN before attach: \(.before_attach_bytes) B untracked, sites under-report by up to this" else "" end)
      + (if (.dropped_allocations // 0) > 0 then "\n  WARN stack table full: \(.dropped_allocations) allocations in no site" else "" end)
      + (if (.suppressed_blocks // 0) > 0 then "\n  suppressed \(.suppressed_bytes) B in \(.suppressed_blocks) runtime blocks" else "" end)
      + ([.leaks[] | select(.direct > 0)] | sort_by(-.direct) | .[:5] | map("\n  leak \(.direct) B \(.kind) \(.site)") | join(""))
      + ([.held[]?] | .[:3] | map("\n  held \(.site) << \(.held_by)") | join(""))' "$d/report.json"
  grep -h "WARN untracked" "$d"/py.*.txt 2>/dev/null | sed 's/^ */  --py /' || true
}

case ${1:-} in
  run)
    d=${2:?usage: ht.sh run DIR [flags] -- CMD...}; shift 2
    mem=${HT_MEM:-$(( $(awk '/MemAvailable/ {print $2}' /proc/meminfo) / 2 ))K}
    # clear only an earlier run: a directory with anything else in it is not ours to delete
    if [[ -e $d ]]; then
      other=$(find "$d" -mindepth 1 -maxdepth 1 ! -name 'heaptide.*' ! -name 'py.*' ! -name 'pyheld.*' ! -name 'report.*' ! -name root ! -name '.sym.*' -print -quit)
      [[ -z $other ]] || { echo "FAIL run: $d holds $other, not a heaptide run: pick another DIR"; exit 2; }
      rm -rf "$d"
    fi
    rc=0
    cap=(systemd-run --user -q --scope -p MemoryMax="$mem" -p MemorySwapMax=0)
    "${cap[@]}" true 2>/dev/null || { cap=(); mem="none (no systemd user session: the run is not capped)"; }
    "${cap[@]}" "$ht" run --out "$d" "$@" 2> "$d.run.err" || rc=$?
    ls "$d"/heaptide.*.bin >/dev/null 2>&1 || { echo "FAIL run: no report written (exit $rc; killed at HT_MEM=$mem? see $d.run.err)"; exit 2; }
    echo "run exit $rc, cap $mem"
    report "$d"; summary "$d" ;;
  summary) summary "${2:?usage: ht.sh summary DIR}" ;;
  prove)
    a=${2:?usage: ht.sh prove BEFORE AFTER [SITE]} b=${3:?usage: ht.sh prove BEFORE AFTER [SITE]} s=${4:-}
    for x in "$a" "$b"; do [[ -s $x/report.json ]] || report "$x"; done
    q='[.[] | .leaks[] | select(.direct > 0) | select(.site | contains($s))] | group_by(.site) | map({(.[0].site): (map(.direct) | add)}) | add // {}'
    jq -rn --arg s "$s" --slurpfile A "$a/report.json" --slurpfile B "$b/report.json" "
      (\$A[0] | $q) as \$x | (\$B[0] | $q) as \$y | ((\$x + \$y) | keys[]) as \$k |
      \"\(\$x[\$k] // 0) -> \(\$y[\$k] // 0) B  \(\$k)\""
    "$ht" diff "$a" "$b" | tail -n +1 || true ;;
  *) sed -n 4,9p "$0" | sed 's/^# //'; exit 2 ;;
esac
