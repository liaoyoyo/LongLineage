#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_agent_task_negative.sh GOVERNANCE REPO_ROOT" >&2
    exit 2
fi

governance="$1"
repo_root="$(cd "$2" && pwd)"

copy_repo_evidence() {
    local scratch="$1"
    local evidence_path="$2"
    if [[ -z "$evidence_path" || "$evidence_path" == /* ||
          "$evidence_path" == "." || "$evidence_path" == ./* ||
          "$evidence_path" == ../* || "$evidence_path" == */../* ||
          "$evidence_path" == */.. ]]; then
        echo "ERROR: unsafe repository evidence path in baseline task: $evidence_path" >&2
        return 1
    fi
    mkdir -p "$scratch/$(dirname "$evidence_path")"
    cp -- "$repo_root/$evidence_path" "$scratch/$evidence_path" || return 1
}

make_scratch_repo() {
    local scratch
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-task-negative.XXXXXX")"
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
    cp "$repo_root/state/audits/"*.json "$scratch/state/audits/"
    for task_path in "$repo_root"/state/tasks/archive/*.json; do
        [[ -e "$task_path" ]] || continue
        cp "$task_path" "$scratch/state/tasks/archive/$(basename "$task_path")"
        while IFS= read -r evidence_path; do
            copy_repo_evidence "$scratch" "$evidence_path" || return 1
        done < <(jq -r '.evidence[]? | .evidence_path // empty' "$task_path")
    done
    while IFS= read -r evidence_path; do
        mkdir -p "$scratch/$(dirname "$evidence_path")"
        cp "$repo_root/$evidence_path" "$scratch/$evidence_path"
    done < <(jq -r '.phases[].evidence[].path' "$repo_root/state/phase_ledger.json" | LC_ALL=C sort -u)

    local now
    local expiry
    now="$(date --iso-8601=seconds)"
    expiry="$(date -d '+30 minutes' --iso-8601=seconds)"
    jq --arg now "$now" --arg expiry "$expiry" '
        .status = "IN_PROGRESS" |
        .owner_agent_id = "codex:root" |
        .lease_state = "ACTIVE" |
        .lease_expires_at = $expiry |
        .heartbeat_at = $now |
        .updated_at = $now |
        .depends_on = [] |
        .blockers = []
    ' "$repo_root/state/tasks/active/20260719-foundation.json" \
        >"$scratch/state/tasks/active/20260719-foundation.json"
    for task_path in "$repo_root"/state/tasks/active/*.json; do
        if [[ "$(basename "$task_path")" == "20260719-foundation.json" ]]; then
            continue
        fi
        jq '
            .status = "BLOCKED" |
            .lease_state = "RELEASED" |
            .blockers = if (.blockers | length) == 0
                        then ["SCRATCH_BASELINE_BLOCKER"]
                        else .blockers
            end
        ' "$task_path" >"$scratch/state/tasks/active/$(basename "$task_path")"
        while IFS= read -r evidence_path; do
            copy_repo_evidence "$scratch" "$evidence_path" || return 1
        done < <(jq -r '.evidence[]? | .evidence_path // empty' "$task_path")
    done
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

unknown_scratch="$(make_scratch_repo)"
jq '.unexpected_field = "must-fail-closed"' \
    "$unknown_scratch/state/tasks/active/20260719-foundation.json" \
    >"$unknown_scratch/task.json"
mv "$unknown_scratch/task.json" \
    "$unknown_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$unknown_scratch" "unknown task field" agent_task_unknown_field_negative

alias_scratch="$(make_scratch_repo)"
jq '.allowed_paths[0].path = "./.ai"' \
    "$alias_scratch/state/tasks/active/20260719-foundation.json" >"$alias_scratch/task.json"
mv "$alias_scratch/task.json" "$alias_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$alias_scratch" "not a normalized non-root relative path" task_path_alias_negative

root_scratch="$(make_scratch_repo)"
jq '.allowed_paths[0].path = "."' \
    "$root_scratch/state/tasks/active/20260719-foundation.json" >"$root_scratch/task.json"
mv "$root_scratch/task.json" "$root_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$root_scratch" "not a normalized non-root relative path" task_root_claim_negative

nested_scratch="$(make_scratch_repo)"
jq '.allowed_paths += [{"path": ".ai/templates/task.json", "kind": "FILE"}]' \
    "$nested_scratch/state/tasks/active/20260719-foundation.json" >"$nested_scratch/task.json"
mv "$nested_scratch/task.json" "$nested_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$nested_scratch" "write-set overlap" task_nested_claim_negative

kind_scratch="$(make_scratch_repo)"
jq '.allowed_paths[0] = {"path": "docs", "kind": "FILE"}' \
    "$kind_scratch/state/tasks/active/20260719-foundation.json" >"$kind_scratch/task.json"
mv "$kind_scratch/task.json" "$kind_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$kind_scratch" "kind differs from the existing target" task_claim_kind_negative

symlink_scratch="$(make_scratch_repo)"
ln -s /tmp "$symlink_scratch/symlinked"
jq '.allowed_paths[0] = {"path": "symlinked/new-file", "kind": "FILE"}' \
    "$symlink_scratch/state/tasks/active/20260719-foundation.json" >"$symlink_scratch/task.json"
mv "$symlink_scratch/task.json" "$symlink_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$symlink_scratch" "traverses a symlink" task_symlink_claim_negative

dependency_scratch="$(make_scratch_repo)"
jq '.depends_on = ["20260719-missing"]' \
    "$dependency_scratch/state/tasks/active/20260719-foundation.json" >"$dependency_scratch/task.json"
mv "$dependency_scratch/task.json" "$dependency_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$dependency_scratch" "dependency does not exist" task_missing_dependency_negative

parent_cycle_scratch="$(make_scratch_repo)"
jq '
    .task_id = "20260719-child" |
    .owner_agent_id = "codex:child" |
    .parent_task_id = "20260719-foundation" |
    .allowed_paths = [{"path": "child-only", "kind": "DIRECTORY_TREE"}]
' "$parent_cycle_scratch/state/tasks/active/20260719-foundation.json" \
    >"$parent_cycle_scratch/state/tasks/active/20260719-child.json"
jq '.parent_task_id = "20260719-child"' \
    "$parent_cycle_scratch/state/tasks/active/20260719-foundation.json" \
    >"$parent_cycle_scratch/task.json"
mv "$parent_cycle_scratch/task.json" \
    "$parent_cycle_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$parent_cycle_scratch" "contains a cycle" task_parent_cycle_negative

dependency_cycle_scratch="$(make_scratch_repo)"
jq '
    .status = "BLOCKED" |
    .lease_state = "RELEASED" |
    .depends_on = ["20260719-child"] |
    .blockers = ["SYNTHETIC_BLOCKER"]
' "$dependency_cycle_scratch/state/tasks/active/20260719-foundation.json" \
    >"$dependency_cycle_scratch/task.json"
mv "$dependency_cycle_scratch/task.json" \
    "$dependency_cycle_scratch/state/tasks/active/20260719-foundation.json"
jq '
    .task_id = "20260719-child" |
    .owner_agent_id = "codex:child" |
    .status = "BLOCKED" |
    .lease_state = "RELEASED" |
    .depends_on = ["20260719-foundation"] |
    .allowed_paths = [{"path": "child-only", "kind": "DIRECTORY_TREE"}] |
    .blockers = ["SYNTHETIC_BLOCKER"]
' "$dependency_cycle_scratch/state/tasks/active/20260719-foundation.json" \
    >"$dependency_cycle_scratch/state/tasks/active/20260719-child.json"
expect_state_failure "$dependency_cycle_scratch" "contains a cycle" task_dependency_cycle_negative

future_scratch="$(make_scratch_repo)"
future_heartbeat="$(date -d '+2 hours' --iso-8601=seconds)"
future_expiry="$(date -d '+3 hours' --iso-8601=seconds)"
jq --arg heartbeat "$future_heartbeat" --arg expiry "$future_expiry" '
    .heartbeat_at = $heartbeat |
    .updated_at = $heartbeat |
    .lease_expires_at = $expiry
' "$future_scratch/state/tasks/active/20260719-foundation.json" >"$future_scratch/task.json"
mv "$future_scratch/task.json" "$future_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$future_scratch" "heartbeat/update/lease window" task_future_heartbeat_negative

stale_scratch="$(make_scratch_repo)"
stale_heartbeat="$(date -d '-2 hours' --iso-8601=seconds)"
stale_expiry="$(date -d '+2 hours' --iso-8601=seconds)"
now="$(date --iso-8601=seconds)"
jq --arg heartbeat "$stale_heartbeat" --arg expiry "$stale_expiry" --arg now "$now" '
    .heartbeat_at = $heartbeat |
    .updated_at = $now |
    .lease_expires_at = $expiry
' "$stale_scratch/state/tasks/active/20260719-foundation.json" >"$stale_scratch/task.json"
mv "$stale_scratch/task.json" "$stale_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$stale_scratch" "recent heartbeat" task_stale_heartbeat_negative

expired_scratch="$(make_scratch_repo)"
expired_future="$(date -d '+1 hour' --iso-8601=seconds)"
jq --arg expiry "$expired_future" '
    .status = "BLOCKED" |
    .lease_state = "EXPIRED" |
    .lease_expires_at = $expiry |
    .blockers = ["SYNTHETIC_BLOCKER"]
' "$expired_scratch/state/tasks/active/20260719-foundation.json" >"$expired_scratch/task.json"
mv "$expired_scratch/task.json" "$expired_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$expired_scratch" "past expiry" task_future_expired_lease_negative

planned_scratch="$(make_scratch_repo)"
jq '
    .status = "PLANNED" |
    .lease_state = "UNASSIGNED" |
    .lease_expires_at = null |
    .heartbeat_at = null
' "$planned_scratch/state/tasks/active/20260719-foundation.json" >"$planned_scratch/task.json"
mv "$planned_scratch/task.json" "$planned_scratch/state/tasks/active/20260719-foundation.json"
expect_state_failure "$planned_scratch" "invalid unassigned lease" task_assigned_planned_negative

echo "agent_task_negative: PASS"
