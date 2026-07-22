#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="${LONGLINEAGE_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$repo_root"

lock="containers/versions.lock.tsv"
dockerfile="containers/Dockerfile"
workflow=".github/workflows/ci.yml"
[[ -f "$lock" && -f "$dockerfile" && -f "$workflow" ]] || {
    echo "DEPENDENCY LOCK FAIL: lock, Dockerfile or CI workflow absent" >&2
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

expected_header=$'component\tversion\tsource\tsha256_or_digest'
[[ "$(sed -n '1p' "$lock")" == "$expected_header" ]] || {
    echo "DEPENDENCY LOCK FAIL: unexpected TSV header" >&2
    exit 1
}

base_digest="$(awk -F '\t' '$1=="ubuntu_base"{print $4}' "$lock")"
boost_version="$(awk -F '\t' '$1=="boost"{print $2}' "$lock")"
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
for manifest_check in "sort -c" "uniq -d" "sha256sum" 'cmp "$runtime" "$live"'; do
    grep -Fq "$manifest_check" "$workflow" || {
        echo "DEPENDENCY LOCK FAIL: image package manifest integrity check is incomplete" >&2
        exit 1
    }
done

echo "DEPENDENCY LOCK RESULT: PASS base=${base_digest} boost=${boost_version} htslib=${htslib_version} apt=resolved-and-captured"
