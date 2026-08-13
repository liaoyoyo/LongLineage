#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lock_file="$repo_root/containers/versions.lock.tsv"
manifest_file="$repo_root/provenance/source_to_target_manifest.json"
receipt_file="$repo_root/docs/release/PUBLIC_SAFETY_RECEIPT.json"
default_output="$repo_root/SBOM.spdx.json"
mode=write
output="$default_output"

if [[ "${1:-}" == "--check" ]]; then
    mode=check
    shift
fi
if [[ $# -gt 1 ]]; then
    echo "usage: generate_sbom_spdx.sh [--check] [OUTPUT]" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    output="$1"
fi

for required in "$lock_file" "$manifest_file" "$receipt_file"; do
    [[ -f "$required" ]] || {
        echo "SPDX SBOM FAIL: missing input ${required#"$repo_root/"}" >&2
        exit 1
    }
done

candidate="$(jq -er '.candidate_commit | select(test("^[0-9a-f]{40}$"))' "$receipt_file")"
created_at="$(jq -er '.sbom_created_at | select(test("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$"))' "$receipt_file")"
document_seed="$({
    sha256sum "$lock_file" "$manifest_file" "${BASH_SOURCE[0]}" | awk '{print $1}'
    printf '%s\n' "$candidate" "$created_at"
} | sha256sum | awk '{print $1}')"
document_namespace="https://github.com/liaoyoyo/LongLineage/sbom/${document_seed}"

scratch="$(mktemp "${TMPDIR:-/tmp}/longlineage-sbom.XXXXXX")"
trap 'rm -f "$scratch"' EXIT

jq -S -n \
    --arg candidate "$candidate" \
    --arg created_at "$created_at" \
    --arg namespace "$document_namespace" \
    --rawfile lock "$lock_file" \
    --slurpfile manifest "$manifest_file" '
    def safe_id:
        gsub("_"; "-") | gsub("[^A-Za-z0-9.-]"; "-");
    def dependency_rows:
        ($lock | split("\n") | .[1:] | map(select(length > 0) | split("\t"))) |
        map({component: .[0], version: .[1], source: .[2], digest: .[3]});
    def dependency_packages:
        dependency_rows |
        map({
            SPDXID: ("SPDXRef-Dependency-" + (.component | safe_id)),
            name: .component,
            versionInfo: .version,
            downloadLocation: "NOASSERTION",
            filesAnalyzed: false,
            licenseConcluded: "NOASSERTION",
            licenseDeclared: "NOASSERTION",
            copyrightText: "NOASSERTION",
            comment: ("Inventory source=" + .source + "; digest_or_resolution=" + .digest +
                      "; license review pending")
        });
    def origin_packages:
        $manifest[0].mappings |
        map({
            SPDXID: ("SPDXRef-Origin-" + (.origin_id | safe_id)),
            name: ("InterSubMod:" + .origin_id),
            versionInfo: (.source_commit // "UNRESOLVED"),
            downloadLocation: "NOASSERTION",
            filesAnalyzed: false,
            licenseConcluded: "NOASSERTION",
            licenseDeclared: "NOASSERTION",
            copyrightText: "NOASSERTION",
            checksums: [{algorithm: "SHA256", checksumValue: .source_sha256}],
            comment: ("source_path=" + .source_path + "; source_replay_status=" +
                      .source_replay_status + "; license_disposition=" + .license_disposition +
                      "; license_evidence=" + .license_evidence),
            annotations: [{
                annotationDate: $created_at,
                annotationType: "REVIEW",
                annotator: "Tool: LongLineage deterministic SPDX generator",
                comment: ("reviewer_id=" + .reviewer_id + "; reviewed_at=" + .reviewed_at +
                          "; review_scope=" + .review_scope)
            }]
        });
    def root_package:
        {
            SPDXID: "SPDXRef-Package-LongLineage",
            name: "LongLineage",
            versionInfo: ("research-preview-" + $candidate),
            downloadLocation: "NOASSERTION",
            filesAnalyzed: false,
            licenseConcluded: "GPL-3.0-only",
            licenseDeclared: "GPL-3.0-only",
            copyrightText: "NOASSERTION",
            comment: "Private research-preview inventory; not a production or public-release approval"
        };
    def relationships:
        ([dependency_rows[] |
            {
                spdxElementId: "SPDXRef-Package-LongLineage",
                relationshipType: "DEPENDS_ON",
                relatedSpdxElement: ("SPDXRef-Dependency-" + (.component | safe_id)),
                comment: "Static lock inventory; runtime/build role and license require final review"
            }] +
         [$manifest[0].mappings[] |
            {
                spdxElementId: "SPDXRef-Package-LongLineage",
                relationshipType: "GENERATED_FROM",
                relatedSpdxElement: ("SPDXRef-Origin-" + (.origin_id | safe_id)),
                comment: ("reuse=" + .reuse + "; transformation=" + .transformation)
            }]) | sort_by(.relatedSpdxElement, .relationshipType);
    {
        spdxVersion: "SPDX-2.3",
        dataLicense: "CC0-1.0",
        SPDXID: "SPDXRef-DOCUMENT",
        name: "LongLineage-private-research-preview",
        documentNamespace: $namespace,
        creationInfo: {
            created: $created_at,
            creators: ["Tool: LongLineage deterministic SPDX generator"]
        },
        documentDescribes: ["SPDXRef-Package-LongLineage"],
        packages: ([root_package] + dependency_packages + origin_packages | sort_by(.SPDXID)),
        relationships: relationships,
        comment: "License/source-origin audit remains pending; NOASSERTION is intentional and fail-closed."
    }
' >"$scratch"

if [[ "$mode" == "check" ]]; then
    [[ -f "$output" ]] || {
        echo "SPDX SBOM CHECK FAIL: checked output is absent: ${output#"$repo_root/"}" >&2
        exit 1
    }
    if ! cmp -s "$scratch" "$output"; then
        echo "SPDX SBOM CHECK FAIL: checked output is not reproducible: ${output#"$repo_root/"}" >&2
        exit 1
    fi
    echo "SPDX SBOM CHECK PASS: output=${output#"$repo_root/"} sha256=$(sha256sum "$output" | awk '{print $1}')"
    exit 0
fi

install -m 0644 "$scratch" "$output"
echo "SPDX SBOM WRITE PASS: output=${output#"$repo_root/"} sha256=$(sha256sum "$output" | awk '{print $1}')"
