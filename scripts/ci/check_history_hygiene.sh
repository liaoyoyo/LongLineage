#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: check_history_hygiene.sh BASE_REF [HEAD_REF]" >&2
    exit 2
fi

base_ref="$1"
head_ref="${2:-HEAD}"
maximum_bytes=1048576
failures=0
blob_count=0
commit_count=0
declare -A scanned_blobs=()

fail() {
    echo "HISTORY HYGIENE FAIL: $*" >&2
    failures=$((failures + 1))
}

git rev-parse --verify "${base_ref}^{commit}" >/dev/null 2>&1 || {
    echo "HISTORY HYGIENE ERROR: base ref is not a commit: $base_ref" >&2
    exit 2
}
git rev-parse --verify "${head_ref}^{commit}" >/dev/null 2>&1 || {
    echo "HISTORY HYGIENE ERROR: head ref is not a commit: $head_ref" >&2
    exit 2
}
if ! git merge-base --is-ancestor "$base_ref" "$head_ref"; then
    echo "HISTORY HYGIENE ERROR: base is not an ancestor of head: $base_ref -> $head_ref" >&2
    exit 2
fi

while IFS= read -r commit; do
    [[ -n "$commit" ]] || continue
    commit_count=$((commit_count + 1))
    while IFS=$'\t' read -r metadata path; do
        [[ -n "$metadata" && -n "$path" ]] || continue
        mode="${metadata%% *}"
        if [[ "$mode" == "120000" ]]; then
            fail "symbolic link appears in introduced commit $commit: $path"
        fi
    done < <(git ls-tree -r "$commit")
done < <(git rev-list --reverse "${base_ref}..${head_ref}")

while IFS=' ' read -r object_id path; do
    [[ -n "$object_id" && -n "${path:-}" ]] || continue
    [[ "$(git cat-file -t "$object_id")" == "blob" ]] || continue

    case "$path" in
        *.bam|*.bai|*.cram|*.crai|*.vcf|*.vcf.gz|*.bcf|*.tbi|*.csi|*.fastq|*.fastq.gz|*.pod5)
            fail "real-genomics file extension appears in introduced history: $path"
            ;;
    esac

    if [[ -n "${scanned_blobs[$object_id]:-}" ]]; then
        continue
    fi
    scanned_blobs[$object_id]=1
    blob_count=$((blob_count + 1))
    size="$(git cat-file -s "$object_id")"
    if ((size > maximum_bytes)); then
        fail "introduced blob $object_id is ${size} bytes; ceiling is ${maximum_bytes}: $path"
    fi
    if git cat-file blob "$object_id" | grep -IEn \
        '/(big|bip)[0-9]+_disk(/|[^[:alnum:]_]|$)|/home/[^ /]+/|/Users/[^ /]+/' >/dev/null; then
        fail "absolute private path appears in introduced blob: $path ($object_id)"
    fi
    if git cat-file blob "$object_id" | grep -IEn \
        '(gh[pousr]_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|-----BEGIN ([A-Z ]+ )?PRIVATE KEY-----|sk-(proj-)?[A-Za-z0-9_-]{20,}|(password|passwd|access_token|api_key)[[:space:]]*[:=][[:space:]]*["'"'"'][^"'"'"']{8,})' \
        >/dev/null; then
        fail "credential-like payload appears in introduced blob: $path ($object_id)"
    fi
    if git cat-file blob "$object_id" | grep -IEn \
        '(^|[^[:alnum:]_])chr([1-9]|1[0-9]|2[0-2]|X|Y|M|MT):[0-9]+|^chr([1-9]|1[0-9]|2[0-2]|X|Y|M|MT)[[:space:]]+[0-9]+' \
        >/dev/null; then
        fail "real-coordinate-shaped payload appears in introduced blob: $path ($object_id)"
    fi
    if [[ "$path" == ".gitattributes" ]] &&
       git cat-file blob "$object_id" | grep -IEin 'filter=lfs|diff=lfs|merge=lfs' >/dev/null; then
        fail "Git LFS configuration appears in introduced history: $object_id"
    fi
done < <(git rev-list --objects "${base_ref}..${head_ref}")

if ((failures > 0)); then
    echo "HISTORY HYGIENE RESULT: FAIL commits=${commit_count} blobs=${blob_count} failures=${failures}" >&2
    exit 1
fi
echo "HISTORY HYGIENE RESULT: PASS commits=${commit_count} blobs=${blob_count} maximum_bytes=${maximum_bytes}"
