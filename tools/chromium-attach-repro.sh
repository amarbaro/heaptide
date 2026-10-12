#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# Reproduce "Chromium browser hangs after heaptide attach" on a throwaway browser: its own profile
# and debugging port, never the user's. Run as root (attach needs ptrace under Yama 1); the browser
# itself runs as $SUDO_USER. Puts itself in the background and returns.
#   sudo tools/chromium-attach-repro.sh [snap|nosnap] [SECONDS] [BROWSER | PID:PORT]
# PID:PORT targets a browser already running (the user's): no tabs opened or closed, never killed
# (a hang is captured and left as it is), detached at the end.
# snap: attach, snapshot, watch; nosnap: attach, watch (bisects the snapshot out). Every 5 s the
# debugging port must answer within 3 s; every 10 s a tab is opened and the previous one closed.
# Three misses in a row = hang: thread wait states, gdb backtraces and heaptide's own state are
# written, then the browser is killed. No hang: heaptide detach, then a last port check.
# Writes .work/chromium-repro/<mode>/{log,hang-*.txt}; last line PASS or FAIL.
set -euo pipefail
mode=${1:-snap}; secs=${2:-900}; bin=${3:-/opt/vivaldi/vivaldi-bin}
here=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
user=${SUDO_USER:?run with sudo}; uid=$(id -u "$user")
port=${REPRO_PORT:-9444}; out=$here/.work/chromium-repro/$mode-$port; pid=""; own=1  # REPRO_PORT: runs side by side
[[ $bin =~ ^([0-9]+):([0-9]+)$ ]] && pid=${BASH_REMATCH[1]} port=${BASH_REMATCH[2]} own=0 out=$out-pid$pid
mkdir -p "$out"; chown "$user": "$here/.work/chromium-repro" "$out"
if [[ ${REPRO_FG:-} != 1 ]]; then
  REPRO_FG=1 setsid nohup "$0" "$@" > "$out/log" 2>&1 < /dev/null &
  echo "started in background: tail $out/log"; exit 0
fi
log() { echo "$(date +%T) $*"; }
fail() { log "FAIL $*"; (( own )) && [[ -n $pid ]] && kill -KILL "$pid" 2>/dev/null; chown -R "$user": "$out"; exit 1; }
ok() { curl -s -m 3 "http://127.0.0.1:$port/json/version" > /dev/null; }
if (( own )); then
pkill -KILL -f -- "--user-data-dir=$here/.work/chromium-repro/profile-$mode-$port" 2>/dev/null || true; sleep 1  # an earlier run's browser
prof=$here/.work/chromium-repro/profile-$mode-$port; rm -rf "$prof"; mkdir -p "$prof"; chown "$user": "$prof"
runuser -u "$user" -- env WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$uid \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus setsid "$bin" --user-data-dir="$prof" \
  --remote-debugging-port=$port --no-first-run --no-default-browser-check about:blank > "$out/browser.log" 2>&1 &
for _ in $(seq 60); do ok && break; sleep 0.5; done; ok || fail "browser: port $port never answered ($out/browser.log)"
pid=$(pgrep -o -u "$user" -f -- "--user-data-dir=$prof") || fail "browser: no main process"
fi
kill -0 "$pid" && ok || fail "browser $pid: not running or port $port silent"
log "browser pid $pid, threads $(ls /proc/$pid/task | wc -l)"
"$here/heaptide" attach "$pid" --out "$out/ht" || fail "attach"
log "attached"
if [[ $mode == snap ]]; then  # a skipped snapshot is logged, not fatal: the hang is the question
  sleep 30  # a starting browser has threads running; settled, most sleep in a system call and need no park
  for a in 1 2 3; do
    if "$here/heaptide" snapshot "$out/ht"; then log "snapshot A (try $a)"; break; fi
    log "snapshot try $a skipped: $(grep 'snapshot skipped' "$out/browser.log" | tail -1)"; sleep 10
  done
fi
capture() {
  for t in /proc/$pid/task/*; do echo "$(basename "$t") $(cat "$t/comm") $(cat "$t/wchan")"; done > "$out/hang-wchan.txt" 2>&1 || true
  cp "/proc/$pid/maps" "$out/hang-maps.txt" 2>/dev/null || true
  timeout 90 gdb -p "$pid" -batch -nx -ex 'set pagination off' \
    -ex 'p (int)ready' -ex 'p (int)done' -ex 'p (int)released' -ex 'p (int)parked' -ex 'p (int)nthr' -ex 'p (int)child' \
    -ex 'p *(unsigned char *)&reporting' -ex 'info threads' -ex 'thread apply all bt 40' \
    -ex 'thread apply all p (int)in_alloc' -ex 'thread apply all p (int)guard' \
    -ex 'thread apply all x/4096gx $sp' > "$out/hang-gdb.txt" 2>&1 || true  # raw stacks: gdb cannot unwind a stripped binary
}
last=""; miss=0; t0=$SECONDS; n=0
while (( SECONDS - t0 < secs )); do
  if ok; then miss=0; else miss=$((miss + 1)); log "port miss $miss"; fi
  if (( miss >= 3 )); then
    log "HANG after $((SECONDS - t0)) s: capturing"; capture; (( own )) || log "left running: pid $pid is yours"
    fail "hang after $((SECONDS - t0)) s ($out/hang-*.txt)"
  fi
  if (( own && n++ % 2 == 0 )); then
    id=$(curl -s -m 3 -X PUT "http://127.0.0.1:$port/json/new?https://example.com/?n=$n" | sed -n 's/.*"id": "\([^"]*\)".*/\1/p' | head -1) || true
    [[ -n $last ]] && curl -s -m 3 "http://127.0.0.1:$port/json/close/$last" > /dev/null || true
    last=$id
  fi
  sleep 5
done
log "no hang in $secs s"
"$here/heaptide" detach "$out/ht" || fail "detach"
ok || fail "port dead after detach"
log "detached, port answers, heaptide mapped: $(grep -c libheaptide "/proc/$pid/maps" || true) (inert)"
(( own )) && kill -TERM "$pid" 2>/dev/null || true
chown -R "$user": "$out"
log "PASS $mode $secs s"
