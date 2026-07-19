#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_phase_state_negative.sh GOVERNANCE REPO_ROOT" >&2
    exit 2
fi

governance="$1"
repo_root="$(cd "$2" && pwd)"

make_scratch_repo() {
    local scratch
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-phase-negative.XXXXXX")"
    mkdir -p \
        "$scratch/docs/audits" \
        "$scratch/docs/data" \
        "$scratch/governance" \
        "$scratch/oracle" \
        "$scratch/provenance" \
        "$scratch/schema/core" \
        "$scratch/state/audits" \
        "$scratch/state/tasks/active" \
        "$scratch/state/tasks/archive"
    cp "$repo_root/AGENTS.md" "$scratch/AGENTS.md"
    cp "$repo_root/ROADMAP.md" "$scratch/ROADMAP.md"
    cp "$repo_root/docs/CURRENT_FOCUS.md" "$scratch/docs/CURRENT_FOCUS.md"
    cp "$repo_root/docs/audits/20260719_runtime_solver_audit.md" "$scratch/docs/audits/"
    cp "$repo_root/docs/data/QUERY_GUIDE.md" "$scratch/docs/data/"
    cp "$repo_root/governance/agent_task.schema.json" "$scratch/governance/"
    cp "$repo_root/governance/phase_ledger.schema.json" "$scratch/governance/"
    cp "$repo_root/governance/project_state.schema.json" "$scratch/governance/"
    cp "$repo_root/oracle/authority_manifest.json" "$scratch/oracle/"
    cp "$repo_root/provenance/source_to_target_manifest.json" "$scratch/provenance/"
    cp "$repo_root/schema/catalog.json" "$scratch/schema/catalog.json"
    cp "$repo_root/schema/core/query_response.schema.json" "$scratch/schema/core/"
    cp "$repo_root/schema/core/validation_receipt.schema.json" "$scratch/schema/core/"
    cp "$repo_root/state/project_state.json" "$scratch/state/project_state.json"
    cp "$repo_root/state/phase_ledger.json" "$scratch/state/phase_ledger.json"
    cp "$repo_root/state/tasks/active/20260719-foundation.json" \
        "$scratch/state/tasks/active/"
    echo "$scratch"
}

expect_state_failure() {
    local scratch="$1"
    local expected="$2"
    local label="$3"
    set +e
    local output
    output="$("$governance" check-state --repo "$scratch" 2>&1)"
    local observed=$?
    set -e
    if [[ "$observed" -ne 5 ]] || ! grep -Fq "$expected" <<<"$output"; then
        echo "${label}: expected exit=5 and diagnostic '${expected}', observed=${observed}" >&2
        echo "$output" >&2
        exit 1
    fi
    echo "${label}: PASS scratch=${scratch} exit=${observed}"
}

predecessor_scratch="$(make_scratch_repo)"
jq '.phases[2].predecessors = []' \
    "$predecessor_scratch/state/phase_ledger.json" \
    >"$predecessor_scratch/state/phase_ledger.mutated.json"
mv "$predecessor_scratch/state/phase_ledger.mutated.json" \
    "$predecessor_scratch/state/phase_ledger.json"
expect_state_failure "$predecessor_scratch" "predecessor chain" \
    phase_predecessor_negative

digest_scratch="$(make_scratch_repo)"
jq '
  .phases[0].status = "VERIFIED" |
  .phases[0].blockers = [] |
  (.phases[0].evidence[] | .sha256) = ("0" * 64) |
  (.phases[0].evidence[] | .command) = "synthetic replay command" |
  (.phases[0].evidence[] | .exit_code) = 0
' "$digest_scratch/state/phase_ledger.json" \
    >"$digest_scratch/state/phase_ledger.mutated.json"
mv "$digest_scratch/state/phase_ledger.mutated.json" \
    "$digest_scratch/state/phase_ledger.json"
expect_state_failure "$digest_scratch" "evidence SHA-256 mismatch" \
    phase_forged_verified_negative

mirror_scratch="$(make_scratch_repo)"
sed -i 's/| P1 | IN_PROGRESS |/| P1 | NOT_STARTED |/' \
    "$mirror_scratch/docs/CURRENT_FOCUS.md"
expect_state_failure "$mirror_scratch" "phase statuses drift from ledger" \
    phase_mirror_drift_negative

scope_scratch="$(make_scratch_repo)"
jq '.scope.datasets = 6' "$scope_scratch/state/project_state.json" \
    >"$scope_scratch/state/project_state.mutated.json"
mv "$scope_scratch/state/project_state.mutated.json" \
    "$scope_scratch/state/project_state.json"
expect_state_failure "$scope_scratch" "7 datasets/6 samples/chr1-22" \
    project_scope_negative

echo "phase_state_negative: PASS"
