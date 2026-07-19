#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="${LONGLINEAGE_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
repo_root="$(cd "$repo_root" && pwd)"
cd "$repo_root"

maximum_bytes=1048576
failures=0

fail() {
    echo "HYGIENE FAIL: $*" >&2
    failures=$((failures + 1))
}

list_file="$(mktemp "${TMPDIR:-/tmp}/longlineage-files.XXXXXX")"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    {
        git ls-files -z
        git ls-files --others --exclude-standard -z
    } | sort -zu >"$list_file"
else
    find . \( -type f -o -type l \) \
        -not -path './.git/*' \
        -not -path './build*/*' \
        -not -path './.cache/*' \
        -printf '%P\0' >"$list_file"
fi

while IFS= read -r -d '' path; do
    if [[ -L "$path" ]]; then
        fail "symbolic links are forbidden in the repository payload: $path"
        continue
    fi
    [[ -f "$path" ]] || continue
    size="$(stat -c '%s' "$path")"
    if ((size > maximum_bytes)); then
        fail "$path is ${size} bytes; tracked-file ceiling is ${maximum_bytes}"
    fi

    case "$path" in
        *.bam|*.bai|*.cram|*.crai|*.vcf|*.vcf.gz|*.bcf|*.tbi|*.csi|*.fastq|*.fastq.gz|*.pod5)
            fail "real-genomics file extension is forbidden: $path"
            ;;
    esac

    if grep -Iq . "$path"; then
        if grep -En '/(big|bip)[0-9]+_disk/|/home/[^ /]+/|/Users/[^ /]+/' "$path" >/dev/null; then
            fail "absolute private path detected: $path"
        fi
        if grep -Ein \
            '(gh[pousr]_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|-----BEGIN ([A-Z ]+ )?PRIVATE KEY-----|sk-(proj-)?[A-Za-z0-9_-]{20,}|(password|passwd|access_token|api_key)[[:space:]]*[:=][[:space:]]*["'"'"'][^"'"'"']{8,})' \
            "$path" >/dev/null; then
            fail "credential-like payload detected: $path"
        fi
        if grep -Ein \
            '(^|[^[:alnum:]_])chr([1-9]|1[0-9]|2[0-2]|X|Y|M|MT):[0-9]+|^chr([1-9]|1[0-9]|2[0-2]|X|Y|M|MT)[[:space:]]+[0-9]+' \
            "$path" >/dev/null; then
            fail "real-coordinate-shaped payload detected: $path"
        fi
    fi
done <"$list_file"

if [[ -f .gitattributes ]] && grep -Ein 'filter=lfs|diff=lfs|merge=lfs' .gitattributes >/dev/null; then
    fail "Git LFS is forbidden by repository policy"
fi

if ((failures > 0)); then
    echo "HYGIENE RESULT: FAIL failures=${failures}" >&2
    exit 1
fi
echo "HYGIENE RESULT: PASS maximum_bytes=${maximum_bytes}"
