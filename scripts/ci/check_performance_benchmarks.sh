#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
schema="$repo_root/governance/performance_benchmark.schema.json"
validator="/usr/bin/jsonschema"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-performance-records.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

[[ -x "$validator" ]] || {
    echo "PERFORMANCE RECORD FAIL: /usr/bin/jsonschema is required" >&2
    exit 1
}

shopt -s nullglob
records=("$repo_root"/state/benchmarks/*.json)
if [[ "${#records[@]}" -eq 0 ]]; then
    echo "PERFORMANCE RECORD FAIL: no benchmark records found" >&2
    exit 1
fi

for record in "${records[@]}"; do
    "$validator" -i "$record" "$schema" >/dev/null
    jq -e '
        def absolute: if . < 0 then -. else . end;
        def median:
            sort as $sorted
            | ($sorted | length) as $count
            | if ($count % 2) == 1
              then $sorted[(($count / 2) | floor)]
              else (($sorted[($count / 2) - 1] + $sorted[$count / 2]) / 2)
              end;
        (.trials.before_seconds | median) as $before
        | (.trials.after_seconds | median) as $after
        | (.workload.repetitions_per_variant == (.trials.before_seconds | length))
          and (.workload.repetitions_per_variant == (.trials.after_seconds | length))
          and ((.result.before_median_seconds - $before) | absolute < 1e-12)
          and ((.result.after_median_seconds - $after) | absolute < 1e-12)
          and ((.result.relative_wall_reduction_percent
                - (100 * ($before - $after) / $before)) | absolute < 1e-9)
          and (.production_claim_allowed == false)
    ' "$record" >/dev/null

    baseline_commit="$(jq -r '.source.baseline_commit' "$record")"
    while IFS=$'\t' read -r path expected; do
        observed="$(git -C "$repo_root" show "$baseline_commit:$path" | sha256sum | awk '{print $1}')"
        [[ "$observed" == "$expected" ]] || {
            echo "PERFORMANCE RECORD FAIL: baseline SHA mismatch: $path" >&2
            exit 1
        }
    done < <(jq -r '.source.baseline_files[] | [.path,.sha256] | @tsv' "$record")

    while IFS=$'\t' read -r path expected; do
        observed="$(sha256sum "$repo_root/$path" | awk '{print $1}')"
        [[ "$observed" == "$expected" ]] || {
            echo "PERFORMANCE RECORD FAIL: candidate SHA mismatch: $path" >&2
            exit 1
        }
    done < <(jq -r '.source.candidate_files[] | [.path,.sha256] | @tsv' "$record")

    harness_path="$(jq -r '.source.harness_path' "$record")"
    harness_sha="$(jq -r '.source.harness_sha256' "$record")"
    [[ "$(sha256sum "$repo_root/$harness_path" | awk '{print $1}')" == "$harness_sha" ]] || {
        echo "PERFORMANCE RECORD FAIL: harness SHA mismatch: $harness_path" >&2
        exit 1
    }

    jq '.production_claim_allowed = true' "$record" >"$scratch/invalid-production-claim.json"
    if "$validator" -i "$scratch/invalid-production-claim.json" "$schema" >/dev/null 2>&1; then
        echo "PERFORMANCE RECORD FAIL: production-claim tamper was accepted" >&2
        exit 1
    fi
    echo "PERFORMANCE RECORD PASS: ${record#"$repo_root"/}"
done

echo "PERFORMANCE RECORD RESULT: PASS records=${#records[@]}"
