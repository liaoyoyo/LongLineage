#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build-release}"

"$repo_root/scripts/ci/check_all.sh" "$build_dir"
"$repo_root/scripts/ci/check_gate_test_coverage.sh" "$build_dir" --strict

if ! jq -e 'all(.phases[]; .status == "VERIFIED" and (.evidence | length) > 0)' \
    "$repo_root/state/phase_ledger.json" >/dev/null; then
    echo "RELEASE GATE BLOCKED: P0-P8 are not all VERIFIED with evidence" >&2
    exit 1
fi

if ! jq -e '
    .attestation_state == "READY" and
    (.blockers | length) == 0 and
    all(.phases[];
        .status == "VERIFIED" and
        (.evidence | length) > 0 and
        all(.evidence[]; .exit_code == 0))
' "$repo_root/state/release_attestation.json" >/dev/null; then
    echo "RELEASE GATE BLOCKED: immutable P3/P4/P5 attestation is not READY" >&2
    exit 1
fi

if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
   [[ -n "$(git -C "$repo_root" status --short)" ]]; then
    echo "RELEASE GATE BLOCKED: worktree is dirty" >&2
    exit 1
fi

echo "RELEASE GATE RESULT: PASS"
