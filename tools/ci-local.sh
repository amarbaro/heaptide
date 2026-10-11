#!/usr/bin/env bash
# Runs .github/workflows/cpu.yml's steps on a clean export of HEAD in the image CI names (ubuntu-24.04),
# before a push. The steps are read from the workflow, never copied: apt steps are baked into a cached
# image (keyed by their text), the rest run as the calling user, so .work/ stays deletable.
set -euo pipefail
cd "$(dirname "$0")/.."
wf=.github/workflows/cpu.yml; src=.work/ci; log=.work/ci.log
python3 - "$wf" > .work/ci-steps.sh <<'EOF' || { echo "FAIL steps: cannot read $wf"; exit 1; }
import sys, yaml
job = next(iter(yaml.safe_load(open(sys.argv[1]))["jobs"].values()))
img = job["runs-on"].replace("ubuntu-", "ubuntu:")
apt = [s["run"].replace("sudo ", "") for s in job["steps"] if "apt-get" in s.get("run", "")]
rest = [s["run"] for s in job["steps"] if "run" in s and "apt-get" not in s["run"]]
print(f"img={img!r}")
print("apt=" + repr(" && ".join(apt)))
print("rest=" + repr(" && ".join(rest)))
EOF
source .work/ci-steps.sh
tag=heaptide-ci:$(printf '%s\n%s' "$img" "$apt" | sha256sum | cut -c1-12)
docker image inspect "$tag" >/dev/null 2>&1 || printf 'FROM %s\nRUN export DEBIAN_FRONTEND=noninteractive && %s && rm -rf /var/lib/apt/lists/*\n' "$img" "$apt" \
  | docker build -q -t "$tag" - >/dev/null || { echo "FAIL image: $tag"; exit 1; }
rm -rf "$src"; mkdir -p "$src" .work/ci-pip; git archive HEAD | tar -x -C "$src"
rc=0; docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD/.work/ci-pip:/tmp/.cache/pip" \
  -v "$PWD/$src:/src" -w /src "$tag" bash -c "set -euo pipefail; gcc --version | head -1; $rest" > "$log" 2>&1 || rc=$?
grep -E '^(FAIL|PASS)' "$log" | tail -3 || true  # display only: a run that never reached the suite has none
(( rc == 0 )) || { echo "FAIL ci-local: exit $rc (log $log)"; exit 1; }
echo "ci-local: OK ($(git rev-parse --short HEAD), $img)"
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
