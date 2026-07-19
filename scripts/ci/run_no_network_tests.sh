#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build}"
ctest_bin="${CTEST_BIN:-/usr/bin/ctest}"

[[ -f "$build_dir/CTestTestfile.cmake" ]] || {
    echo "NO-NETWORK FAIL: not a configured test tree: $build_dir" >&2
    exit 1
}

run_ctest=("$ctest_bin" --test-dir "$build_dir" --output-on-failure)

if [[ "${LONGLINEAGE_NETWORK_ISOLATED:-0}" == "1" ]]; then
    "${run_ctest[@]}"
    echo "NO-NETWORK RESULT: PASS authority=external-isolated-environment"
    exit 0
fi

if command -v unshare >/dev/null &&
   unshare --user --map-root-user --net true >/dev/null 2>&1; then
    unshare --user --map-root-user --net "${run_ctest[@]}"
    echo "NO-NETWORK RESULT: PASS authority=linux-network-namespace"
    exit 0
fi

if ! command -v strace >/dev/null; then
    echo "NO-NETWORK FAIL: neither network namespace nor strace enforcement is available" >&2
    exit 1
fi

trace_prefix="$build_dir/Testing/Temporary/no-network-trace-$$"
mkdir -p "$(dirname "$trace_prefix")"
strace -ff -qq -e trace=network -o "$trace_prefix" "${run_ctest[@]}"
if grep -E 'AF_INET|AF_INET6' "${trace_prefix}"* >/dev/null 2>&1; then
    echo "NO-NETWORK FAIL: a test attempted an IPv4/IPv6 syscall; trace=${trace_prefix}" >&2
    exit 1
fi
echo "NO-NETWORK RESULT: PASS authority=strace-no-AF_INET trace=${trace_prefix}"
