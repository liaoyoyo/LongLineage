#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="${LONGLINEAGE_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$repo_root"

lock="containers/versions.lock.tsv"
dockerfile="containers/Dockerfile"
dockerignore=".dockerignore"
cmake_file="CMakeLists.txt"
workflow=".github/workflows/ci.yml"
[[ -f "$lock" && -f "$dockerfile" && -f "$dockerignore" &&
   -f "$cmake_file" && -f "$workflow" ]] || {
    echo "DEPENDENCY LOCK FAIL: lock, container provenance or CI workflow input absent" >&2
    exit 1
}
synthetic_job="$(
    awk '
        /^  synthetic-build-test:/ {inside = 1}
        /^  pinned-container:/ {inside = 0}
        inside {print}
    ' "$workflow"
)"
[[ -n "$synthetic_job" ]] || {
    echo "DEPENDENCY LOCK FAIL: synthetic-build-test job is absent" >&2
    exit 1
}
pinned_job="$(
    awk '
        /^  pinned-container:/ {inside = 1; next}
        inside && /^  [A-Za-z0-9_-]+:/ {exit}
        inside {print}
    ' "$workflow"
)"
[[ -n "$pinned_job" ]] || {
    echo "DEPENDENCY LOCK FAIL: pinned-container job is absent" >&2
    exit 1
}

expected_header=$'component\tversion\tsource\tsha256_or_digest'
[[ "$(sed -n '1p' "$lock")" == "$expected_header" ]] || {
    echo "DEPENDENCY LOCK FAIL: unexpected TSV header" >&2
    exit 1
}

base_digest="$(awk -F '\t' '$1=="ubuntu_base"{print $4}' "$lock")"
boost_version="$(awk -F '\t' '$1=="boost"{print $2}' "$lock")"
git_version="$(awk -F '\t' '$1=="git"{print $2}' "$lock")"
htslib_version="$(awk -F '\t' '$1=="htslib"{print $2}' "$lock")"
htslib_sha="$(awk -F '\t' '$1=="htslib"{print $4}' "$lock")"
apt_resolution="$(awk -F '\t' '$1=="apt_resolution"{print $2 "\t" $4}' "$lock")"

[[ "$base_digest" =~ ^sha256:[0-9a-f]{64}$ ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed base digest" >&2
    exit 1
}
[[ "$htslib_version" == "1.18" && "$htslib_sha" =~ ^[0-9a-f]{64}$ ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed HTSlib pin" >&2
    exit 1
}
[[ "$boost_version" == "1.74.x" ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed Boost version policy" >&2
    exit 1
}
[[ "$git_version" == "2.34.x" ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed Git version policy" >&2
    exit 1
}
[[ "$apt_resolution" == $'jammy-current-at-image-build\tcaptured-in-image' ]] || {
    echo "DEPENDENCY LOCK FAIL: apt resolution policy is not explicit" >&2
    exit 1
}
[[ "$(tail -n +2 "$lock" | cut -f1 | sort | uniq -d | wc -l)" -eq 0 ]] || {
    echo "DEPENDENCY LOCK FAIL: duplicate component row" >&2
    exit 1
}
grep -Fq "$base_digest" "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: Dockerfile base digest differs from lock" >&2
    exit 1
}
grep -Fq "HTSLIB_VERSION=1.18" "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: Dockerfile HTSlib version differs from lock" >&2
    exit 1
}
grep -Fq "$htslib_sha" "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: Dockerfile HTSlib SHA differs from lock" >&2
    exit 1
}
grep -Fq "libboost-dev" "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: builder image does not install libboost-dev" >&2
    exit 1
}
grep -Fxq ".git" "$dockerignore" || {
    echo "DEPENDENCY LOCK FAIL: Docker context does not exclude .git" >&2
    exit 1
}
if grep -Eq '^[[:space:]]*COPY[[:space:]]+.*[[:space:]]\.git([/[:space:]]|$)' "$dockerfile"; then
    echo "DEPENDENCY LOCK FAIL: Dockerfile must not copy .git" >&2
    exit 1
fi
grep -Fq "ARG LONGLINEAGE_GIT_COMMIT" "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: builder does not declare the source commit argument" >&2
    exit 1
}
for provenance_guard in \
        '${#LONGLINEAGE_GIT_COMMIT}" -ne 40' \
        '0000000000000000000000000000000000000000' \
        '*[!0-9a-f]*'; do
    grep -Fq "$provenance_guard" "$dockerfile" || {
        echo "DEPENDENCY LOCK FAIL: builder source commit validation is incomplete" >&2
        exit 1
    }
done
grep -Fq -- '-DLONGLINEAGE_GIT_COMMIT="${LONGLINEAGE_GIT_COMMIT}"' "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: builder does not pass the source commit to CMake" >&2
    exit 1
}
grep -Fq -- '-DLONGLINEAGE_ALLOW_EXTERNAL_GIT_COMMIT_ASSERTION=ON' "$dockerfile" || {
    echo "DEPENDENCY LOCK FAIL: .git-free builder does not explicitly acknowledge its external commit assertion" >&2
    exit 1
}
commit_arg_line="$(grep -n '^ARG LONGLINEAGE_GIT_COMMIT$' "$dockerfile" | cut -d: -f1)"
dependency_install_line="$(grep -n '&& make install$' "$dockerfile" | tail -n1 | cut -d: -f1)"
[[ "$commit_arg_line" =~ ^[0-9]+$ && "$dependency_install_line" =~ ^[0-9]+$ &&
   "$commit_arg_line" -gt "$dependency_install_line" ]] || {
    echo "DEPENDENCY LOCK FAIL: source commit argument invalidates stable dependency layers" >&2
    exit 1
}
grep -Fq 'set(LONGLINEAGE_GIT_COMMIT "" CACHE STRING' "$cmake_file" || {
    echo "DEPENDENCY LOCK FAIL: CMake does not expose the explicit source commit cache input" >&2
    exit 1
}
grep -Fq 'Explicit LONGLINEAGE_GIT_COMMIT must be a 40-character lowercase non-zero SHA-1' \
    "$cmake_file" || {
    echo "DEPENDENCY LOCK FAIL: CMake does not reject an invalid explicit source commit" >&2
    exit 1
}
for checkout_guard in \
        'Explicit LONGLINEAGE_GIT_COMMIT does not match checkout HEAD' \
        'Explicit LONGLINEAGE_GIT_COMMIT requires a clean tracked and untracked checkout' \
        'A .git-free source commit assertion requires explicit trusted-builder opt-in'; do
    grep -Fq "$checkout_guard" "$cmake_file" || {
        echo "DEPENDENCY LOCK FAIL: CMake source checkout binding is incomplete" >&2
        exit 1
    }
done
apt_install_commands="$(
    awk '
        function emit_if_apt_install() {
            if (logical ~ /(^|[[:space:]&;])apt-get[[:space:]]+install([[:space:]]|$)/) {
                print logical
            }
        }
        {
            line = $0
            sub(/[[:space:]]*\\[[:space:]]*$/, "", line)
            logical = (logical == "" ? line : logical " " line)
            if ($0 ~ /\\[[:space:]]*$/) {
                next
            }
            emit_if_apt_install()
            logical = ""
        }
        END {
            if (logical != "") {
                emit_if_apt_install()
            }
        }
    ' "$dockerfile"
)"
[[ -n "$apt_install_commands" ]] || {
    echo "DEPENDENCY LOCK FAIL: Dockerfile has no apt install command to audit" >&2
    exit 1
}
builder_install_command="$(head -n1 <<<"$apt_install_commands")"
grep -Eq '(^|[[:space:]\\])git([[:space:]\\]|$)' <<<"$builder_install_command" || {
    echo "DEPENDENCY LOCK FAIL: builder image does not install git" >&2
    exit 1
}
if grep -Eq '(^|[[:space:]\\])[a-z0-9][a-z0-9+.-]*(:[a-z0-9-]+)?=[^[:space:]\\]+' \
    <<<"$apt_install_commands"; then
    echo "DEPENDENCY LOCK FAIL: mutable apt archive must not use retired exact package pins" >&2
    exit 1
fi
if grep -Fq $'\tapt-version' "$lock"; then
    echo "DEPENDENCY LOCK FAIL: lock claims unavailable exact apt versions" >&2
    exit 1
fi
[[ "$(grep -Fc "dpkg-query -W" "$dockerfile")" -eq 2 ]] || {
    echo "DEPENDENCY LOCK FAIL: builder/runtime package manifests are not both captured" >&2
    exit 1
}
for manifest in builder-apt-packages.tsv runtime-apt-packages.tsv; do
    grep -Fq "$manifest" "$dockerfile" || {
        echo "DEPENDENCY LOCK FAIL: missing image package manifest $manifest" >&2
        exit 1
    }
done
grep -Eq '^[[:space:]]+build-essential clang clang-format-14 cmake curl' <<<"$synthetic_job" || {
    echo "DEPENDENCY LOCK FAIL: hosted runner does not install cmake explicitly" >&2
    exit 1
}
grep -Fq "libboost-dev" <<<"$synthetic_job" || {
    echo "DEPENDENCY LOCK FAIL: hosted runner does not install libboost-dev" >&2
    exit 1
}
grep -Fq "test -x /usr/bin/cmake" <<<"$synthetic_job" || {
    echo "DEPENDENCY LOCK FAIL: hosted runner does not probe apt-owned cmake" >&2
    exit 1
}
grep -Fq "/usr/bin/cmake --version" <<<"$synthetic_job" || {
    echo "DEPENDENCY LOCK FAIL: hosted runner does not record apt-owned cmake" >&2
    exit 1
}
[[ "$(grep -Fc "/usr/bin/cmake" <<<"$synthetic_job")" -ge 4 ]] || {
    echo "DEPENDENCY LOCK FAIL: workflow does not consistently use apt-owned cmake" >&2
    exit 1
}
grep -Fq "fetch-depth: 0" <<<"$synthetic_job" || {
    echo "DEPENDENCY LOCK FAIL: hosted runner does not fetch benchmark baseline history" >&2
    exit 1
}
grep -Fq -- '--build-arg LONGLINEAGE_GIT_COMMIT="${GITHUB_SHA}"' <<<"$pinned_job" || {
    echo "DEPENDENCY LOCK FAIL: pinned-container build does not inject GITHUB_SHA" >&2
    exit 1
}
for hosted_binding in \
        'verify_source_binding() {' \
        'git rev-parse --verify HEAD' \
        'test "$actual_commit" = "$GITHUB_SHA"' \
        'git cat-file -e "${GITHUB_SHA}^{commit}"' \
        'git status --porcelain=v1 --untracked-files=all'; do
    grep -Fq "$hosted_binding" <<<"$pinned_job" || {
        echo "DEPENDENCY LOCK FAIL: hosted source commit attestation is incomplete" >&2
        exit 1
    }
done
mapfile -t source_binding_calls < <(
    grep -nE '^[[:space:]]+verify_source_binding$' <<<"$pinned_job" | cut -d: -f1
)
docker_build_line="$(grep -nE '^[[:space:]]+docker build' <<<"$pinned_job" | cut -d: -f1)"
[[ "${#source_binding_calls[@]}" -eq 2 && "$docker_build_line" =~ ^[0-9]+$ &&
   "${source_binding_calls[0]}" -lt "$docker_build_line" &&
   "$docker_build_line" -lt "${source_binding_calls[1]}" ]] || {
    echo "DEPENDENCY LOCK FAIL: hosted source commit attestation must bracket the container build" >&2
    exit 1
}
for manifest_check in "sort -c" "uniq -d" "sha256sum" 'cmp "$runtime" "$live"'; do
    grep -Fq "$manifest_check" "$workflow" || {
        echo "DEPENDENCY LOCK FAIL: image package manifest integrity check is incomplete" >&2
        exit 1
    }
done

echo "DEPENDENCY LOCK RESULT: PASS base=${base_digest} boost=${boost_version} git=${git_version} htslib=${htslib_version} apt=resolved-and-captured commit=clean-head-verified-and-externally-asserted"
