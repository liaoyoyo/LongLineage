#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo "usage: check_public_preview_gate.sh [HEAD_REF]" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
head_ref="${1:-HEAD}"
manifest="$repo_root/provenance/source_to_target_manifest.json"
receipt="$repo_root/docs/release/PUBLIC_SAFETY_RECEIPT.json"
sbom="$repo_root/SBOM.spdx.json"
blockers=0

block() {
    echo "PUBLIC PREVIEW BLOCKER: $*" >&2
    blockers=$((blockers + 1))
}

required_files=(
    provenance/source_to_target_manifest.json
    schema/core/source_to_target_manifest.schema.json
    docs/release/PUBLIC_PREVIEW.md
    docs/release/PUBLIC_SAFETY_RECEIPT.json
    docs/release/CAPABILITY_MATRIX.md
    THIRD_PARTY_NOTICES.md
    SBOM.spdx.json
    scripts/release/generate_sbom_spdx.sh
    scripts/ci/check_history_hygiene.sh
)
missing_required=0
for relative in "${required_files[@]}"; do
    if [[ ! -f "$repo_root/$relative" ]]; then
        block "missing_required_file=$relative"
        missing_required=1
    fi
done
if ((missing_required > 0)); then
    echo "PUBLIC PREVIEW GATE RESULT: FAIL blockers=$blockers" >&2
    exit 1
fi

audit_base="$(jq -r '.history_hygiene.merge_base // empty' "$receipt")"
history_base_valid=true
if ! [[ "$audit_base" =~ ^[0-9a-f]{40}$ ]]; then
    block "history_audit_base=INVALID_OR_MISSING"
    history_base_valid=false
elif ! git -C "$repo_root" cat-file -e "${audit_base}^{commit}" 2>/dev/null; then
    block "history_audit_base_unavailable=$audit_base"
    history_base_valid=false
elif ! git -C "$repo_root" merge-base --is-ancestor "$audit_base" "$head_ref"; then
    block "history_audit_base_not_ancestor=base:$audit_base,head:$head_ref"
    history_base_valid=false
fi

if ! /usr/bin/jsonschema -i "$manifest" \
    "$repo_root/schema/core/source_to_target_manifest.schema.json" >/dev/null; then
    block "source_manifest_schema=FAIL"
fi

repository_license_status="$(jq -r '.license_review_status' "$manifest")"
unresolved_source_rows="$(jq '[.mappings[] | select(.source_replay_status == "HASH_NOT_FOUND")] | length' "$manifest")"
unapproved_license_rows="$(jq '[.mappings[] | select(.license_disposition != "APPROVED_FOR_PUBLIC_RELEASE")] | length' "$manifest")"
rejected_license_rows="$(jq '[.mappings[] | select(.license_disposition == "REJECTED_FOR_PUBLIC_RELEASE")] | length' "$manifest")"
target_rows="$(jq '[.mappings[] | select(.target_presence == "PRESENT" and .target_digest_kind == "FILE_SHA256")] | length' "$manifest")"

if [[ "$repository_license_status" != "APPROVED_FOR_PUBLIC_RELEASE" ]]; then
    block "repository_license_review=$repository_license_status"
fi
if ((unresolved_source_rows > 0)); then
    block "unresolved_source_rows=$unresolved_source_rows"
fi
if ((unapproved_license_rows > 0)); then
    block "unapproved_source_license_rows=$unapproved_license_rows"
fi
if ((rejected_license_rows > 0)); then
    block "rejected_source_license_rows=$rejected_license_rows"
fi

dependency_noassertion="$(jq '[.packages[] |
    select(.SPDXID | startswith("SPDXRef-Dependency-")) |
    select(.licenseDeclared == "NOASSERTION" or .licenseConcluded == "NOASSERTION")
] | length' "$sbom")"
if ((dependency_noassertion > 0)); then
    block "dependency_license_noassertion=$dependency_noassertion"
fi
if ! "$repo_root/scripts/release/generate_sbom_spdx.sh" --check "$sbom" >/dev/null; then
    block "deterministic_sbom=FAIL"
fi

receipt_consistent=true
if ! jq -e \
    --argjson unresolved "$unresolved_source_rows" \
    --argjson targets "$target_rows" \
    --argjson pending "$unapproved_license_rows" '
    .gate_status == "FAIL_CLOSED" and
    .required_verdict == "KEEP_PRIVATE_NO_TAG_NO_RELEASE" and
    .visibility_incident.containment_final_visibility == "PRIVATE" and
    .source_replay.unresolved_rows == $unresolved and
    .source_replay.target_digest_matches == $targets and
    .license_review.row_dispositions_pending == $pending
' "$receipt" >/dev/null; then
    receipt_consistent=false
    block "public_safety_receipt=INCONSISTENT"
fi

for phase in P3 P4 P5 P7 P8; do
    if ! jq -e --arg phase "$phase" '.phases[] | select(.id == $phase and .status == "BLOCKED")' \
        "$repo_root/state/phase_ledger.json" >/dev/null; then
        block "phase_boundary_not_blocked=$phase"
    fi
    if ! grep -Fq "$phase" "$repo_root/docs/release/PUBLIC_PREVIEW.md"; then
        block "public_preview_missing_phase=$phase"
    fi
done
for marker in "NOT_READY" "RESEARCH PREVIEW" "NON-PRODUCTION" 'exit `6`' "47/47"; do
    if ! grep -Fq "$marker" "$repo_root/docs/release/PUBLIC_PREVIEW.md"; then
        block "public_preview_missing_marker=$marker"
    fi
done

if [[ "$history_base_valid" == true ]]; then
    set +e
    history_output="$(
        cd "$repo_root"
        "$repo_root/scripts/ci/check_history_hygiene.sh" "$audit_base" "$head_ref" 2>&1
    )"
    history_exit=$?
    set -e
else
    history_output=""
    history_exit=2
fi
history_finding_count=""
if ((history_exit != 0)); then
    history_summary="$(
        grep -F 'HISTORY HYGIENE RESULT:' <<<"$history_output" | tail -n1 || true
    )"
    block "history_hygiene_exit=$history_exit ${history_summary:-summary_missing}"
    if [[ "$history_summary" =~ failures=([0-9]+) ]]; then
        history_finding_count="${BASH_REMATCH[1]}"
    else
        block "history_hygiene_finding_count=UNAVAILABLE"
    fi
else
    history_finding_count=0
fi

receipt_history_findings="$(jq '.history_hygiene.finding_count' "$receipt")"
if [[ "$history_finding_count" != "$receipt_history_findings" ]]; then
    block "history_receipt_count_mismatch=observed:${history_finding_count:-UNAVAILABLE},receipt:$receipt_history_findings"
fi

if [[ -n "$(git -C "$repo_root" tag --points-at "$head_ref")" ]]; then
    block "candidate_has_local_tag=true"
fi

echo "PUBLIC PREVIEW GATE OBSERVATION: mappings=$(jq '.mappings | length' "$manifest") targets=$target_rows unresolved=$unresolved_source_rows license_pending=$unapproved_license_rows receipt_consistent=$receipt_consistent"
echo "PUBLIC PREVIEW GATE OBSERVATION: history_audit_base=$audit_base head_ref=$head_ref"
echo "PUBLIC PREVIEW GATE OBSERVATION: visibility_verification=EXTERNAL_REQUIRED no_network_call_performed=true"

if ((blockers > 0)); then
    echo "PUBLIC PREVIEW GATE RESULT: FAIL blockers=$blockers" >&2
    exit 1
fi

echo "PUBLIC PREVIEW GATE RESULT: PASS"
