#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
envelope="${1:-}"

if ! git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "AUDIT SOURCE FAIL: repository has no Git history" >&2
    exit 1
fi

if [[ -n "$envelope" ]]; then
    envelopes=("$envelope")
else
    shopt -s nullglob
    envelopes=("$repo_root"/state/audits/*.json)
    shopt -u nullglob
fi
if [[ "${#envelopes[@]}" -eq 0 ]]; then
    echo "AUDIT SOURCE FAIL: no machine audit envelope exists" >&2
    exit 1
fi

for envelope_path in "${envelopes[@]}"; do
    [[ -f "$envelope_path" ]] || {
        echo "AUDIT SOURCE FAIL: envelope is absent: $envelope_path" >&2
        exit 1
    }
    snapshot_id="$(jq -r '.snapshot_id // empty' "$envelope_path")"
    snapshot_kind="$(jq -r '.source_snapshot_kind // empty' "$envelope_path")"
    commit="$(jq -r '.source_commit // empty' "$envelope_path")"
    declared_tree="$(jq -r '.source_tree_sha256 // empty' "$envelope_path")"
    source_dirty="$(jq -r '.source_dirty' "$envelope_path")"
    if [[ "$snapshot_kind" != "GIT_COMMIT" || "$source_dirty" != "false" ||
          ! "$commit" =~ ^([0-9a-f]{40}|[0-9a-f]{64})$ ||
          ! "$declared_tree" =~ ^[0-9a-f]{64}$ ]]; then
        echo "AUDIT SOURCE FAIL: envelope must bind a clean GIT_COMMIT snapshot: $envelope_path" >&2
        exit 1
    fi
    git -C "$repo_root" cat-file -e "${commit}^{commit}" 2>/dev/null || {
        echo "AUDIT SOURCE FAIL: source commit is not locally resolvable: $commit" >&2
        exit 1
    }

    manifest_unsorted="$(mktemp "${TMPDIR:-/tmp}/longlineage-audit-tree.unsorted.XXXXXX")"
    manifest_sorted="$(mktemp "${TMPDIR:-/tmp}/longlineage-audit-tree.sorted.XXXXXX")"
    selected=0
    while IFS= read -r -d '' relative; do
        if [[ "$relative" == *$'\t'* || "$relative" == *$'\n'* ]]; then
            echo "AUDIT SOURCE FAIL: Git path contains a forbidden TAB/LF: $relative" >&2
            exit 1
        fi
        if ! jq -e --arg path "$relative" '
            def covers($prefix):
                $prefix == "." or $path == $prefix or ($path | startswith($prefix + "/"));
            any(.scope.included_paths[]; covers(.)) and
            all(.scope.excluded_paths[]; (covers(.) | not))
        ' "$envelope_path" >/dev/null; then
            continue
        fi
        bytes="$(git -C "$repo_root" cat-file -s "${commit}:${relative}")"
        sha="$(git -C "$repo_root" cat-file blob "${commit}:${relative}" | sha256sum | awk '{print $1}')"
        printf '%s\t%s\t%s\n' "$relative" "$bytes" "$sha" >>"$manifest_unsorted"
        selected=$((selected + 1))
    done < <(git -C "$repo_root" ls-tree -r --name-only -z "$commit")
    if [[ "$selected" -eq 0 ]]; then
        echo "AUDIT SOURCE FAIL: scope selected zero Git blobs: $snapshot_id" >&2
        exit 1
    fi
    LC_ALL=C sort "$manifest_unsorted" >"$manifest_sorted"
    observed_tree="$(sha256sum "$manifest_sorted" | awk '{print $1}')"
    if [[ "$observed_tree" != "$declared_tree" ]]; then
        echo "AUDIT SOURCE FAIL: tree digest mismatch snapshot=${snapshot_id} expected=${declared_tree} observed=${observed_tree}" >&2
        exit 1
    fi
    echo "AUDIT SOURCE PASS: snapshot=${snapshot_id} commit=${commit} blobs=${selected} tree_sha256=${observed_tree}"
done

echo "AUDIT SOURCE RESULT: PASS envelopes=${#envelopes[@]}"
