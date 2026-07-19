#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

lock="containers/versions.lock.tsv"
dockerfile="containers/Dockerfile"
[[ -f "$lock" && -f "$dockerfile" ]] || {
    echo "DEPENDENCY LOCK FAIL: lock or Dockerfile absent" >&2
    exit 1
}

expected_header=$'component\tversion\tsource\tsha256_or_digest'
[[ "$(sed -n '1p' "$lock")" == "$expected_header" ]] || {
    echo "DEPENDENCY LOCK FAIL: unexpected TSV header" >&2
    exit 1
}

base_digest="$(awk -F '\t' '$1=="ubuntu_base"{print $4}' "$lock")"
htslib_version="$(awk -F '\t' '$1=="htslib"{print $2}' "$lock")"
htslib_sha="$(awk -F '\t' '$1=="htslib"{print $4}' "$lock")"

[[ "$base_digest" =~ ^sha256:[0-9a-f]{64}$ ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed base digest" >&2
    exit 1
}
[[ "$htslib_version" == "1.18" && "$htslib_sha" =~ ^[0-9a-f]{64}$ ]] || {
    echo "DEPENDENCY LOCK FAIL: malformed HTSlib pin" >&2
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

echo "DEPENDENCY LOCK RESULT: PASS base=${base_digest} htslib=${htslib_version}"
