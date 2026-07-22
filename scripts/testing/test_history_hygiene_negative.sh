#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-history-hygiene.XXXXXX")"
git -C "$scratch" init -q
git -C "$scratch" config user.name "LongLineage synthetic test"
git -C "$scratch" config user.email "synthetic@example.invalid"
printf 'safe baseline\n' >"$scratch/README.md"
git -C "$scratch" add README.md
git -C "$scratch" commit -q -m baseline
base="$(git -C "$scratch" rev-parse HEAD)"

printf '/%s%s\n' 'bip9' '_disk' >"$scratch/private-mount-root.txt"
git -C "$scratch" add private-mount-root.txt
git -C "$scratch" commit -q -m synthetic-private-root

set +e
output="$(cd "$scratch" && "$repo_root/scripts/ci/check_history_hygiene.sh" "$base" HEAD 2>&1)"
observed=$?
set -e

if [[ "$observed" -eq 0 ]]; then
    echo "history hygiene negative: mount root without a trailing slash was missed" >&2
    echo "$output" >&2
    exit 1
fi
if ! grep -Fq 'private-mount-root.txt' <<<"$output"; then
    echo "history hygiene negative: failure did not identify fixture" >&2
    echo "$output" >&2
    exit 1
fi

echo "history_hygiene_negative: PASS scratch=$scratch"
