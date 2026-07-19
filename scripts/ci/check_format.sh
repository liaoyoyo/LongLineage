#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
formatter="${1:-${LONGLINEAGE_CLANG_FORMAT:-clang-format-14}}"

command -v "$formatter" >/dev/null 2>&1 || {
    echo "FORMAT FAIL: required formatter is unavailable: $formatter" >&2
    exit 1
}

mapfile -d '' source_files < <(
    find "$repo_root/apps" "$repo_root/include" "$repo_root/src" "$repo_root/tests" \
        -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 |
        sort -z
)

if ((${#source_files[@]} == 0)); then
    echo "FORMAT FAIL: no C++ source files found" >&2
    exit 1
fi

"$formatter" --dry-run --Werror "${source_files[@]}"
echo "FORMAT RESULT: PASS formatter=$("$formatter" --version | sed -n '1p') files=${#source_files[@]}"
