#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

expected_license_sha256="8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903"
failures=0

fail() {
    echo "SPDX FAIL: $*" >&2
    failures=$((failures + 1))
}

if [[ ! -f LICENSE ]]; then
    fail "canonical LICENSE is absent"
else
    observed="$(sha256sum LICENSE | awk '{print $1}')"
    if [[ "$observed" != "$expected_license_sha256" ]]; then
        fail "LICENSE digest mismatch: expected ${expected_license_sha256}, observed ${observed}"
    fi
fi

while IFS= read -r -d '' path; do
    if ! sed -n '1,5p' "$path" | grep -Fq 'SPDX-License-Identifier: GPL-3.0-only'; then
        fail "missing GPL-3.0-only SPDX header: ${path#./}"
    fi
done < <(
    find . -type f \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.sh' -o -name '*.cmake' \
           -o -name 'CMakeLists.txt' -o -name 'Dockerfile' -o -name '*.yml' -o -name '*.yaml' \) \
        -not -path './build*/*' -not -path './.git/*' -print0
)

if ((failures > 0)); then
    echo "SPDX RESULT: FAIL failures=${failures}" >&2
    exit 1
fi
echo "SPDX RESULT: PASS license_sha256=${expected_license_sha256}"
