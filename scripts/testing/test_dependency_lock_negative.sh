#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$repo_root/scripts/ci/check_dependency_lock.sh"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-dependency-lock-negative.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

mkdir -p "$scratch/.github/workflows" "$scratch/containers" "$scratch/scripts/ci"
cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
cp "$repo_root/containers/versions.lock.tsv" "$scratch/containers/versions.lock.tsv"
cp "$checker" "$scratch/scripts/ci/check_dependency_lock.sh"

LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" >/dev/null

sed -i \
    '0,/ca-certificates \\/s/ca-certificates \\/ca-certificates=0.invalid \\/' \
    "$scratch/containers/Dockerfile"
set +e
pin_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
pin_status=$?
set -e
[[ "$pin_status" -ne 0 && "$pin_output" == *"retired exact package pins"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: exact apt pin was not rejected" >&2
    exit 1
}

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
printf '\nRUN apt-get install -y libssl3:amd64=0.invalid\n' >>"$scratch/containers/Dockerfile"
set +e
inline_pin_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
inline_pin_status=$?
set -e
[[ "$inline_pin_status" -ne 0 && "$inline_pin_output" == *"retired exact package pins"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: inline multiarch apt pin was not rejected" >&2
    exit 1
}

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i 's/clang-format-14 cmake curl/clang-format-14 curl/' \
    "$scratch/.github/workflows/ci.yml"
set +e
cmake_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
cmake_status=$?
set -e
[[ "$cmake_status" -ne 0 && "$cmake_output" == *"does not install cmake explicitly"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: missing hosted-runner cmake was not rejected" >&2
    exit 1
}

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
sed -i '/fetch-depth: 0/d' "$scratch/.github/workflows/ci.yml"
set +e
history_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
history_status=$?
set -e
[[ "$history_status" -ne 0 && "$history_output" == *"does not fetch benchmark baseline history"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: shallow hosted checkout was not rejected" >&2
    exit 1
}

echo "DEPENDENCY LOCK NEGATIVE RESULT: PASS"
