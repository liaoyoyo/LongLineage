#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_audit_evidence_negative.sh GOVERNANCE REPO_ROOT" >&2
    exit 2
fi

governance="$1"
repo_root="$(cd "$2" && pwd)"

make_scratch_repo() {
    local scratch
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-audit-negative.XXXXXX")"
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
    while IFS= read -r evidence_path; do
        mkdir -p "$scratch/$(dirname "$evidence_path")"
        cp "$repo_root/$evidence_path" "$scratch/$evidence_path"
    done < <(
        jq -r '.phases[].evidence[] | select(.role != "AUDIT") | .path' \
            "$repo_root/state/phase_ledger.json" | LC_ALL=C sort -u
    )
    jq '(.phases[].evidence) |= map(select(.role != "AUDIT"))' \
        "$repo_root/state/phase_ledger.json" >"$scratch/state/phase_ledger.json"
    jq '.evidence = []' \
        "$repo_root/state/tasks/active/20260719-foundation.json" \
        >"$scratch/state/tasks/active/20260719-foundation.json"
    echo "$scratch"
}

write_envelope() {
    local scratch="$1"
    local snapshot_id="$2"
    jq --arg snapshot "$snapshot_id" '
        .snapshot_id = $snapshot |
        .supersedes = [] |
        .superseded_by = null |
        .supersession_reason = null
    ' "$repo_root/tests/fixtures/governance/audit_evidence.valid.json" \
        >"$scratch/state/audits/${snapshot_id}.json"
}

bind_phase_p2() {
    local scratch="$1"
    local snapshot_id="$2"
    local digest
    digest="$(sha256sum "$scratch/state/audits/${snapshot_id}.json" | awk '{print $1}')"
    jq --arg path "state/audits/${snapshot_id}.json" --arg digest "$digest" '
        .phases[2].evidence = [{
            "role": "AUDIT",
            "path": $path,
            "sha256": $digest,
            "command": "synthetic audit replay",
            "exit_code": 0
        }]
    ' "$scratch/state/phase_ledger.json" >"$scratch/ledger.json"
    mv "$scratch/ledger.json" "$scratch/state/phase_ledger.json"
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

valid_scratch="$(make_scratch_repo)"
write_envelope "$valid_scratch" "20260719-foundation-contract-002"
bind_phase_p2 "$valid_scratch" "20260719-foundation-contract-002"
"$governance" check-state --repo "$valid_scratch" >/dev/null
echo "audit_envelope_positive: PASS scratch=$valid_scratch"

markdown_scratch="$(make_scratch_repo)"
markdown_sha="$(sha256sum "$markdown_scratch/docs/audits/20260719_runtime_solver_audit.md" | awk '{print $1}')"
jq --arg digest "$markdown_sha" '
    .phases[2].evidence[0].role = "AUDIT" |
    .phases[2].evidence[0].sha256 = $digest |
    .phases[2].evidence[0].command = "synthetic replay" |
    .phases[2].evidence[0].exit_code = 0
' "$markdown_scratch/state/phase_ledger.json" >"$markdown_scratch/ledger.json"
mv "$markdown_scratch/ledger.json" "$markdown_scratch/state/phase_ledger.json"
expect_state_failure "$markdown_scratch" "machine envelope under state/audits" audit_markdown_negative

self_scratch="$(make_scratch_repo)"
write_envelope "$self_scratch" "20260719-foundation-contract-002"
jq '.supersedes = [.snapshot_id] | .supersession_reason = "self edge must fail"' \
    "$self_scratch/state/audits/20260719-foundation-contract-002.json" >"$self_scratch/envelope.json"
mv "$self_scratch/envelope.json" \
    "$self_scratch/state/audits/20260719-foundation-contract-002.json"
bind_phase_p2 "$self_scratch" "20260719-foundation-contract-002"
expect_state_failure "$self_scratch" "self edge" audit_self_edge_negative

tips_scratch="$(make_scratch_repo)"
write_envelope "$tips_scratch" "20260719-foundation-contract-002"
write_envelope "$tips_scratch" "20260719-foundation-contract-004"
bind_phase_p2 "$tips_scratch" "20260719-foundation-contract-002"
expect_state_failure "$tips_scratch" "exactly one current" audit_multiple_tips_negative

task_scratch="$(make_scratch_repo)"
write_envelope "$task_scratch" "20260719-foundation-contract-002"
jq '.task_id = "20260719-missing"' \
    "$task_scratch/state/audits/20260719-foundation-contract-002.json" >"$task_scratch/envelope.json"
mv "$task_scratch/envelope.json" \
    "$task_scratch/state/audits/20260719-foundation-contract-002.json"
bind_phase_p2 "$task_scratch" "20260719-foundation-contract-002"
expect_state_failure "$task_scratch" "audit task is unknown" audit_unknown_task_negative

time_scratch="$(make_scratch_repo)"
write_envelope "$time_scratch" "20260719-foundation-contract-002"
jq '
    .commands[0].started_at = "2026-07-19T07:50:48+08:00" |
    .commands[0].completed_at = "2026-07-19T07:50:47+08:00"
' "$time_scratch/state/audits/20260719-foundation-contract-002.json" >"$time_scratch/envelope.json"
mv "$time_scratch/envelope.json" \
    "$time_scratch/state/audits/20260719-foundation-contract-002.json"
bind_phase_p2 "$time_scratch" "20260719-foundation-contract-002"
expect_state_failure "$time_scratch" "time order is invalid" audit_command_time_negative

missing_scratch="$(make_scratch_repo)"
write_envelope "$missing_scratch" "20260719-foundation-contract-002"
jq '
    .supersedes = ["20260719-foundation-contract-001"] |
    .supersession_reason = "missing predecessor must fail"
' "$missing_scratch/state/audits/20260719-foundation-contract-002.json" >"$missing_scratch/envelope.json"
mv "$missing_scratch/envelope.json" \
    "$missing_scratch/state/audits/20260719-foundation-contract-002.json"
bind_phase_p2 "$missing_scratch" "20260719-foundation-contract-002"
expect_state_failure "$missing_scratch" "unresolved or crosses" audit_missing_predecessor_negative

echo "audit_evidence_negative: PASS"
