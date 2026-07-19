#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: test_gate_binding_negative.sh COVERAGE_SCRIPT BUILD_DIR REPO_ROOT" >&2
    exit 2
fi

coverage="$1"
build_dir="$2"
repo_root="$(cd "$3" && pwd)"

expect_binding_failure() {
    local fixture="$1"
    local diagnostic="$2"
    local label="$3"
    set +e
    local output
    output="$(
        "$coverage" "$build_dir" --allow-declared-blocked \
            "$repo_root/tests/fixtures/governance/$fixture" 2>&1
    )"
    local observed=$?
    set -e
    if [[ "$observed" -ne 1 ]] || ! grep -Fq "$diagnostic" <<<"$output"; then
        echo "${label}: expected exit=1 and diagnostic='${diagnostic}', observed=${observed}" >&2
        echo "$output" >&2
        exit 1
    fi
    echo "${label}: PASS exit=${observed} fixture=${fixture}"
}

expect_binding_failure \
    gate_registry.missing_binding.tsv \
    "binding is missing" \
    missing_binding_negative
expect_binding_failure \
    gate_registry.unknown_binding.tsv \
    "unknown_binding=legacy_arbitrary_label" \
    unknown_binding_vocabulary_negative
expect_binding_failure \
    gate_registry.unknown_test.tsv \
    "ctest_id=no_such_stable_test_id" \
    unknown_ctest_target_negative
expect_binding_failure \
    gate_registry.unknown_fixture.tsv \
    "absent, empty, unsafe, or not a regular in-repo fixture" \
    unknown_fixture_target_negative
expect_binding_failure \
    gate_registry.duplicate_binding.tsv \
    "duplicate_binding=fixture:tests/fixtures/governance/missing_required_file.case.json" \
    duplicate_binding_negative

echo "gate_negative_binding_contract: PASS"
