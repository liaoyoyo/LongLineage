#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-hygiene-untracked.XXXXXX")"
git -C "$scratch" init -q
printf 'synthetic forbidden payload\n' >"$scratch/untracked-forbidden.bam"
printf '/%s%s\n' 'big9' '_disk' >"$scratch/private-mount-root.txt"

set +e
output="$(LONGLINEAGE_REPO_ROOT="$scratch" \
    "$repo_root/scripts/ci/check_repo_hygiene.sh" 2>&1)"
observed=$?
set -e

if [[ "$observed" -eq 0 ]]; then
    echo "untracked hygiene negative: forbidden untracked file was missed" >&2
    echo "$output" >&2
    exit 1
fi
if ! grep -Fq 'untracked-forbidden.bam' <<<"$output"; then
    echo "untracked hygiene negative: failure did not identify fixture" >&2
    echo "$output" >&2
    exit 1
fi
if ! grep -Fq 'private-mount-root.txt' <<<"$output"; then
    echo "untracked hygiene negative: mount root without a trailing slash was missed" >&2
    echo "$output" >&2
    exit 1
fi

echo "untracked_hygiene_negative: PASS scratch=$scratch"
