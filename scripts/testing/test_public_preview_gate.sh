#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_public_preview_gate.sh GATE REPO_ROOT" >&2
    exit 2
fi

gate="$1"
repo_root="$(cd "$2" && pwd)"
manifest="$repo_root/provenance/source_to_target_manifest.json"
schema="$repo_root/schema/core/source_to_target_manifest.schema.json"

set +e
/usr/bin/jsonschema -i <(
    jq '.license_review_status = "APPROVED_FOR_PUBLIC_RELEASE"' "$manifest"
) "$schema" >/dev/null 2>&1
false_approval_exit=$?
set -e
if [[ "$false_approval_exit" -eq 0 ]]; then
    echo "PUBLIC PREVIEW REGRESSION FAIL: schema accepted false root license approval" >&2
    exit 1
fi

set +e
output="$("$gate" HEAD 2>&1)"
observed=$?
set -e

if [[ "$observed" -ne 1 ]]; then
    echo "PUBLIC PREVIEW REGRESSION FAIL: expected gate exit 1, observed $observed" >&2
    echo "$output" >&2
    exit 1
fi

for expected in \
    "repository_license_review=PENDING_PUBLIC_RELEASE_AUDIT" \
    "unresolved_source_rows=4" \
    "unapproved_source_license_rows=21" \
    "dependency_license_noassertion=11" \
    "history_hygiene_exit=1" \
    "failures=7" \
    "history_audit_base=5daf50f04cbe233abfade816ce9e0903f6b38954" \
    "PUBLIC PREVIEW GATE RESULT: FAIL"; do
    if ! grep -Fq "$expected" <<<"$output"; then
        echo "PUBLIC PREVIEW REGRESSION FAIL: missing expected blocker: $expected" >&2
        echo "$output" >&2
        exit 1
    fi
done
if grep -Fq "PUBLIC PREVIEW GATE RESULT: PASS" <<<"$output"; then
    echo "PUBLIC PREVIEW REGRESSION FAIL: blocked candidate reported PASS" >&2
    exit 1
fi

echo "PUBLIC PREVIEW REGRESSION PASS: gate_exit=$observed false_approval_exit=$false_approval_exit source_rows=4 license_rows=21 dependency_noassertion=11 history_findings=7"
