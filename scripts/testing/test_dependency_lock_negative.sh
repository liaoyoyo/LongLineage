#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$repo_root/scripts/ci/check_dependency_lock.sh"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-dependency-lock-negative.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

mkdir -p "$scratch/.github/workflows" "$scratch/containers" "$scratch/scripts/ci"
cp "$repo_root/.dockerignore" "$scratch/.dockerignore"
cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
cp "$repo_root/CMakeLists.txt" "$scratch/CMakeLists.txt"
cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
cp "$repo_root/containers/versions.lock.tsv" "$scratch/containers/versions.lock.tsv"
cp "$checker" "$scratch/scripts/ci/check_dependency_lock.sh"

LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" >/dev/null

expect_lock_failure() {
    local description="$1"
    local expected="$2"
    local output
    local status
    set +e
    output="$(LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1)"
    status=$?
    set -e
    [[ "$status" -ne 0 && "$output" == *"$expected"* ]] || {
        echo "DEPENDENCY LOCK NEGATIVE FAIL: $description was not rejected" >&2
        exit 1
    }
}

if actual_head="$(git -C "$repo_root" rev-parse --verify HEAD 2>/dev/null)"; then
    fake_commit="1111111111111111111111111111111111111111"
    if [[ "$fake_commit" == "$actual_head" ]]; then
        fake_commit="2222222222222222222222222222222222222222"
    fi
    set +e
    mismatch_output="$(
        /usr/bin/cmake -S "$repo_root" -B "$scratch/cmake-mismatched-head" \
            -DLONGLINEAGE_GIT_COMMIT="$fake_commit" 2>&1
    )"
    mismatch_status=$?
    set -e
    [[ "$mismatch_status" -ne 0 &&
       "$mismatch_output" == *"does not match checkout HEAD"* ]] || {
        echo "DEPENDENCY LOCK NEGATIVE FAIL: checkout/head commit mismatch was not rejected" >&2
        exit 1
    }
fi

mkdir -p "$scratch/git-free-source"
cp "$repo_root/CMakeLists.txt" "$scratch/git-free-source/CMakeLists.txt"
set +e
git_free_output="$(
    /usr/bin/cmake -S "$scratch/git-free-source" -B "$scratch/cmake-git-free" \
        -DLONGLINEAGE_GIT_COMMIT=1111111111111111111111111111111111111111 2>&1
)"
git_free_status=$?
set -e
[[ "$git_free_status" -ne 0 &&
   "$git_free_output" == *"requires explicit trusted-builder"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: unacknowledged .git-free assertion was not rejected" >&2
    exit 1
}

mkdir -p "$scratch/dirty-git-source"
cp "$repo_root/CMakeLists.txt" "$scratch/dirty-git-source/CMakeLists.txt"
git -C "$scratch/dirty-git-source" init -q
git -C "$scratch/dirty-git-source" config user.name "LongLineage CI"
git -C "$scratch/dirty-git-source" config user.email "ci@longlineage.invalid"
git -C "$scratch/dirty-git-source" add CMakeLists.txt
git -C "$scratch/dirty-git-source" commit -q -m "fixture"
dirty_head="$(git -C "$scratch/dirty-git-source" rev-parse HEAD)"
printf '\n# synthetic dirty-tree mutation\n' >>"$scratch/dirty-git-source/CMakeLists.txt"
set +e
dirty_output="$(
    /usr/bin/cmake -S "$scratch/dirty-git-source" -B "$scratch/cmake-dirty-git" \
        -DLONGLINEAGE_GIT_COMMIT="$dirty_head" 2>&1
)"
dirty_status=$?
set -e
[[ "$dirty_status" -ne 0 &&
   "$dirty_output" == *"requires a clean tracked and untracked"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: dirty checkout with matching HEAD was not rejected" >&2
    exit 1
}

sed -i '/^[[:space:]]*git \\/d' "$scratch/containers/Dockerfile"
expect_lock_failure "missing builder-image Git" "builder image does not install git"

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i '/^ARG LONGLINEAGE_GIT_COMMIT$/d' "$scratch/containers/Dockerfile"
expect_lock_failure "missing Docker source commit argument" "does not declare the source commit argument"

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i '/${#LONGLINEAGE_GIT_COMMIT}/d' "$scratch/containers/Dockerfile"
expect_lock_failure "incomplete Docker source commit validation" "source commit validation is incomplete"

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i '/LONGLINEAGE_ALLOW_EXTERNAL_GIT_COMMIT_ASSERTION=ON/d' "$scratch/containers/Dockerfile"
expect_lock_failure "unacknowledged external commit assertion" "does not explicitly acknowledge"

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i \
    's/set(LONGLINEAGE_GIT_COMMIT "" CACHE STRING/set(LONGLINEAGE_GIT_COMMIT ""/' \
    "$scratch/CMakeLists.txt"
expect_lock_failure "missing CMake source commit cache input" "does not expose the explicit source commit cache input"

cp "$repo_root/CMakeLists.txt" "$scratch/CMakeLists.txt"
sed -i '/--build-arg LONGLINEAGE_GIT_COMMIT=/d' "$scratch/.github/workflows/ci.yml"
expect_lock_failure "missing CI source commit injection" "does not inject GITHUB_SHA"

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
sed -i '/git cat-file -e/d' "$scratch/.github/workflows/ci.yml"
expect_lock_failure "missing hosted commit existence check" "hosted source commit attestation is incomplete"

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
sed -i '/test "$actual_commit" = "$GITHUB_SHA"/d' "$scratch/.github/workflows/ci.yml"
expect_lock_failure "missing hosted HEAD equality check" "hosted source commit attestation is incomplete"

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
awk '
    /^[[:space:]]+verify_source_binding$/ {
        ++calls
        if (calls == 2) {
            next
        }
    }
    {print}
' "$scratch/.github/workflows/ci.yml" >"$scratch/.github/workflows/ci.yml.mutated"
mv "$scratch/.github/workflows/ci.yml.mutated" "$scratch/.github/workflows/ci.yml"
expect_lock_failure "missing post-build source attestation" "must bracket the container build"

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"
sed -i '/^ARG LONGLINEAGE_GIT_COMMIT$/d' "$scratch/containers/Dockerfile"
sed -i '/^ARG HTSLIB_SHA256=/a ARG LONGLINEAGE_GIT_COMMIT' "$scratch/containers/Dockerfile"
expect_lock_failure "commit argument before stable dependency layers" "invalidates stable dependency layers"

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
sed -i '/^\.git$/d' "$scratch/.dockerignore"
expect_lock_failure "Docker .git context leak" "Docker context does not exclude .git"

cp "$repo_root/.dockerignore" "$scratch/.dockerignore"
sed -i 's/^git\t2\.34\.x\t/git\t0.invalid\t/' "$scratch/containers/versions.lock.tsv"
expect_lock_failure "malformed Git version policy" "malformed Git version policy"

cp "$repo_root/containers/versions.lock.tsv" "$scratch/containers/versions.lock.tsv"

sed -i 's/ libboost-dev//' "$scratch/.github/workflows/ci.yml"
set +e
hosted_boost_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
hosted_boost_status=$?
set -e
[[ "$hosted_boost_status" -ne 0 &&
   "$hosted_boost_output" == *"hosted runner does not install libboost-dev"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: missing hosted-runner Boost was not rejected" >&2
    exit 1
}

cp "$repo_root/.github/workflows/ci.yml" "$scratch/.github/workflows/ci.yml"
sed -i '/^[[:space:]]*libboost-dev \\/d' "$scratch/containers/Dockerfile"
set +e
image_boost_output="$(
    LONGLINEAGE_REPO_ROOT="$scratch" "$scratch/scripts/ci/check_dependency_lock.sh" 2>&1
)"
image_boost_status=$?
set -e
[[ "$image_boost_status" -ne 0 &&
   "$image_boost_output" == *"builder image does not install libboost-dev"* ]] || {
    echo "DEPENDENCY LOCK NEGATIVE FAIL: missing builder-image Boost was not rejected" >&2
    exit 1
}

cp "$repo_root/containers/Dockerfile" "$scratch/containers/Dockerfile"

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
