#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# Publish gate, run before any push: clean tree, no attribution trailers, no tracked artifacts,
# personal paths or secrets, copyright headers, the community files, a CHANGELOG line per release note.
set -uo pipefail
cd "$(dirname "$0")/.."
fail=0
say() { echo "FAIL: $*"; fail=1; }
[ -z "$(git status --short)" ] || say "working tree not clean"
git log --format=%B | grep -qiE '^co-authored-by|generated with \[claude' && say "attribution trailer in history"
git ls-files | grep -qE '^(\.work|\.venv|build|dist)/|\.env$' && say "ignored artifact tracked"
git ls-files -z | xargs -0 grep -lE '/home/[a-z]+/|api[_-]?key *=|token *= *["'"'"']|BEGIN (RSA|OPENSSH) PRIVATE' 2>/dev/null | grep -v '^tools/publish-check.sh$' | grep . && say "personal path or secret in tracked files"
for f in heaptide build.sh $(git ls-files 'shim/*.c' 'report/*.mojo' 'py/*.py' 'tests/*' 'tools/*.sh' 'bench/*.c' 'examples/*.c'); do
  grep -q "Copyright" "$f" || say "no copyright header: $f"
done
for f in LICENSE NOTICE README.md CONTRIBUTING.md CODE_OF_CONDUCT.md SECURITY.md CHANGELOG.md; do [ -f "$f" ] || say "missing $f"; done
for n in docs/releases/v*.md; do v=$(basename "$n" .md); grep -q "\[$v\]" CHANGELOG.md || say "CHANGELOG has no line for $v"; done
[ "$fail" = 0 ] && echo "publish-check: OK ($(git rev-parse --short HEAD))"
exit $fail
