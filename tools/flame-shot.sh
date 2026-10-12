#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# A heaptide flame page rendered in a headless Chromium: screenshots in both themes, and a check in the
# page itself (no script error, boxes drawn, search finds FRAME, the inverted view keeps the total, a
# zoom redraws). Needs the cdp-headless, page-shot and cdp-eval tools ($ITOOLS/bin, default ~/iTools).
#   tools/flame-shot.sh DIR [FRAME]   ->  .work/flame-shot/{flame.html,dark.png,light.png,check.json}
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); out=$here/.work/flame-shot; bin=${ITOOLS:-$HOME/iTools}/bin
dir=${1:?usage: tools/flame-shot.sh DIR [FRAME]}; want=${2:-}
for t in cdp-headless page-shot cdp-eval; do [[ -x $bin/$t ]] || { echo "FAIL flame-shot: $bin/$t missing"; exit 1; }; done
mkdir -p "$out"; "$here/heaptide" flame "$dir" -o "$out/flame.html"
port=$("$bin/cdp-headless" --width 1600 --height 1000 | tail -1)
trap '"$bin/cdp-headless" --kill "$port" >/dev/null 2>&1 || true' EXIT
for t in dark light; do "$bin/page-shot" "$port" "file://$out/flame.html?$t" "$out/$t.png" --wait=1.2 > /dev/null; done
"$bin/cdp-eval" --port "$port" --tab flame.html "JSON.stringify(window.__ht.check(${want:+\"$want\"}))" | tail -1 > "$out/check.json"
python3 - "$out/check.json" "$want" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); want = sys.argv[2]; bad = []
if r["errors"]: bad.append(f"script errors: {r['errors']}")
if r["drawn"] < 1: bad.append("nothing drawn")
if r["inverted_total"] != r["total"]: bad.append(f"inverted total {r['inverted_total']} != {r['total']}")
if want and not r.get("search_hits"): bad.append(f"search found no {want}")
print(json.dumps(r))
print("FAIL flame-shot: " + "; ".join(bad) if bad else f"flame-shot: OK ({r['drawn']} boxes, {r['redraw_ms']:.1f} ms redraw)")
sys.exit(1 if bad else 0)
PY
