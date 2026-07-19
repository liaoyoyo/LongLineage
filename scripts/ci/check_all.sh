#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build}"

"$repo_root/scripts/ci/check_repo_hygiene.sh"
"$repo_root/scripts/ci/check_source_boundaries.sh"
"$repo_root/scripts/ci/check_spdx.sh"
"$repo_root/scripts/ci/check_dependency_lock.sh"
"$repo_root/scripts/ci/check_json_schema_fixtures.sh"

governance="$build_dir/bin/longlineage-governance"
[[ -x "$governance" ]] || {
    echo "CHECK ALL FAIL: missing compiled governance binary: $governance" >&2
    exit 1
}
"$governance" check-all --repo "$repo_root"
"$repo_root/scripts/ci/check_gate_test_coverage.sh" \
    "$build_dir" --allow-declared-blocked
"$repo_root/scripts/ci/run_no_network_tests.sh" "$build_dir"

echo "CHECK ALL RESULT: FOUNDATION_PASS build=$build_dir (release-phase blockers remain governed)"
