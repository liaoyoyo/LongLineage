#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build}"

echo "INPUT: $repo_root"
echo "COMMAND: /usr/bin/cmake -S $repo_root -B $build_dir -DCMAKE_BUILD_TYPE=Debug -DLONGLINEAGE_BUILD_TESTS=ON"
echo "OUTPUT: $build_dir"

/usr/bin/cmake -S "$repo_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLONGLINEAGE_BUILD_TESTS=ON \
    -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON
/usr/bin/cmake --build "$build_dir" --parallel "${LONGLINEAGE_BUILD_JOBS:-4}"
"$repo_root/scripts/ci/check_all.sh" "$build_dir"
