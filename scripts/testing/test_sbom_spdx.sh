#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
generator="$repo_root/scripts/release/generate_sbom_spdx.sh"
sbom="$repo_root/SBOM.spdx.json"
manifest="$repo_root/provenance/source_to_target_manifest.json"
lock="$repo_root/containers/versions.lock.tsv"

"$generator" --check "$sbom"

dependency_count="$(awk 'NR > 1 && NF > 0 {count++} END {print count + 0}' "$lock")"
mapping_count="$(jq '.mappings | length' "$manifest")"
expected_packages=$((1 + dependency_count + mapping_count))
pending_rows="$(jq '[.mappings[] | select(.license_disposition != "APPROVED_FOR_PUBLIC_RELEASE")] | length' "$manifest")"

jq -e \
    --argjson expected_packages "$expected_packages" \
    --argjson mapping_count "$mapping_count" \
    --argjson pending_rows "$pending_rows" '
    .spdxVersion == "SPDX-2.3" and
    .dataLicense == "CC0-1.0" and
    (.packages | length) == $expected_packages and
    ([.packages[].SPDXID] | unique | length) == $expected_packages and
    ([.packages[] | select(.SPDXID | startswith("SPDXRef-Origin-"))] | length) == $mapping_count and
    ([.packages[] |
        select(.SPDXID | startswith("SPDXRef-Origin-")) |
        select(.licenseDeclared == "NOASSERTION" and .licenseConcluded == "NOASSERTION")
     ] | length) == $pending_rows and
    ([.packages[] |
        select(.SPDXID | startswith("SPDXRef-Origin-")) |
        .annotations[] | select(.annotationType == "REVIEW")
     ] | length) == $mapping_count and
    ([.relationships[] | select(.relationshipType == "GENERATED_FROM")] | length) == $mapping_count
' "$sbom" >/dev/null

scratch_one="$(mktemp "${TMPDIR:-/tmp}/longlineage-sbom-test-one.XXXXXX")"
scratch_two="$(mktemp "${TMPDIR:-/tmp}/longlineage-sbom-test-two.XXXXXX")"
trap 'rm -f "$scratch_one" "$scratch_two"' EXIT
"$generator" "$scratch_one" >/dev/null
"$generator" "$scratch_two" >/dev/null
cmp "$scratch_one" "$scratch_two"

echo "SPDX SBOM TEST PASS: packages=$expected_packages dependencies=$dependency_count mappings=$mapping_count pending=$pending_rows"
