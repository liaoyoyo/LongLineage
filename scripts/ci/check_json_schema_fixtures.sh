#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
validator="/usr/bin/jsonschema"

[[ -x "$validator" ]] || {
    echo "JSON SCHEMA FIXTURES FAIL: /usr/bin/jsonschema is required" >&2
    exit 1
}

validate() {
    local schema="$1"
    local instance="$2"
    "$validator" -i "$repo_root/$instance" "$repo_root/$schema" >/dev/null
    echo "JSON SCHEMA FIXTURE PASS: $instance"
}

reject() {
    local schema="$1"
    local instance="$2"
    set +e
    "$validator" -i "$repo_root/$instance" "$repo_root/$schema" >/dev/null 2>&1
    local observed=$?
    set -e
    if [[ "$observed" -eq 0 ]]; then
        echo "JSON SCHEMA FIXTURE FAIL: invalid instance accepted: $instance" >&2
        exit 1
    fi
    echo "JSON SCHEMA FIXTURE NEGATIVE PASS: $instance exit=$observed"
}

validate governance/project_state.schema.json state/project_state.json
validate governance/agent_task.schema.json \
    state/tasks/active/20260719-foundation.json
validate governance/agent_task.schema.json .ai/templates/task.json
validate governance/audit_evidence.schema.json \
    tests/fixtures/governance/audit_evidence.valid.json
reject governance/project_state.schema.json \
    tests/fixtures/governance/project_state.invalid_missing_p0_gate.json
reject governance/agent_task.schema.json \
    tests/fixtures/governance/agent_task.invalid_active_without_lease.json
reject governance/audit_evidence.schema.json \
    tests/fixtures/governance/audit_evidence.invalid_missing_tree_digest.json
validate governance/phase_ledger.schema.json state/phase_ledger.json
validate schema/core/release_attestation.schema.json state/release_attestation.json
validate schema/core/source_to_target_manifest.schema.json \
    provenance/source_to_target_manifest.json
reject schema/core/source_to_target_manifest.schema.json \
    tests/fixtures/contracts/source_to_target_manifest.invalid_planned_digest.json
validate schema/core/contract_registry_bindings.schema.json \
    tests/fixtures/contracts/contract_registry_bindings.valid.json
reject schema/core/contract_registry_bindings.schema.json \
    tests/fixtures/contracts/contract_registry_bindings.invalid_digest.json
validate schema/records/group_allele_counts.embedded.schema.json \
    tests/fixtures/contracts/group_allele_counts.valid.json
reject schema/records/group_allele_counts.embedded.schema.json \
    tests/fixtures/contracts/group_allele_counts.invalid_row.json
validate schema/records/compatible_relation_models.embedded.schema.json \
    tests/fixtures/contracts/compatible_relation_models.valid.json
reject schema/records/compatible_relation_models.embedded.schema.json \
    tests/fixtures/contracts/compatible_relation_models.invalid_order.json
validate schema/records/joint_partner_orders.embedded.schema.json \
    tests/fixtures/contracts/joint_partner_orders.valid.json
reject schema/records/joint_partner_orders.embedded.schema.json \
    tests/fixtures/contracts/joint_partner_orders.invalid_duplicate.json
validate schema/core/query_response.schema.json \
    tests/fixtures/contracts/query_response.valid.json
reject schema/core/query_response.schema.json \
    tests/fixtures/contracts/query_response.invalid_partial_count.json
validate schema/records/topology_unit.schema.json \
    tests/fixtures/contracts/topology_unit.valid_abstain.json
validate schema/records/topology_unit.schema.json \
    tests/fixtures/contracts/topology_unit.valid_unique_winner.json
reject schema/records/topology_unit.schema.json \
    tests/fixtures/contracts/topology_unit.invalid_tied_winner.json

if ! jq -e '
    (.start0 | type) == "number" and
    (.end0 | type) == "number" and
    .start0 >= 0 and .end0 > .start0
' "$repo_root/tests/fixtures/contracts/site_reads_interval.valid.json" >/dev/null; then
    echo "INTERVAL0 FIXTURE FAIL: valid fixture rejected" >&2
    exit 1
fi
for invalid_interval in \
    tests/fixtures/contracts/site_reads_interval.invalid_equal.json \
    tests/fixtures/contracts/site_reads_interval.invalid_reversed.json; do
    if jq -e '
        (.start0 | type) == "number" and
        (.end0 | type) == "number" and
        .start0 >= 0 and .end0 > .start0
    ' "$repo_root/$invalid_interval" >/dev/null; then
        echo "INTERVAL0 FIXTURE FAIL: invalid fixture accepted: $invalid_interval" >&2
        exit 1
    fi
    echo "INTERVAL0 FIXTURE NEGATIVE PASS: $invalid_interval"
done
echo "INTERVAL0 FIXTURE PASS: tests/fixtures/contracts/site_reads_interval.valid.json"

echo "JSON SCHEMA FIXTURES RESULT: PASS"
