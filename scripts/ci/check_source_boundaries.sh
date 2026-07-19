#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

failures=0
fail() {
    echo "BOUNDARY FAIL: $*" >&2
    failures=$((failures + 1))
}

while IFS= read -r -d '' python_file; do
    case "$python_file" in
        ./presentation/*) ;;
        *) fail "Python is allowed only under presentation/: ${python_file#./}" ;;
    esac
done < <(find . -type f -name '*.py' -not -path './build*/*' -print0)

production_paths=(
    apps/longlineage_main.cpp
    src/io
    src/artifact
    src/runtime
    src/solver
    src/science
)
for path in "${production_paths[@]}"; do
    [[ -e "$path" ]] || continue
    if grep -Rni --include='*.cpp' --include='*.hpp' 'truth' "$path" >/dev/null; then
        fail "evaluation-only vocabulary appears in production source boundary: $path"
    fi
done

if grep -En 'longlineage_add_cli\(longlineage_validator.*longlineage_core' CMakeLists.txt >/dev/null; then
    fail "independent validator target links producer core"
fi
while IFS= read -r -d '' link_command; do
    if grep -Fq 'liblonglineage_core' "$link_command"; then
        fail "compiled independent validator link command contains producer core: $link_command"
    fi
done < <(find . -path './build*/CMakeFiles/longlineage_validator.dir/link.txt' -print0)
if grep -RniE \
    '#include[[:space:]]+[<"].*(science|solver|topology|statistics)/' \
    apps/validate_main.cpp src/validation 2>/dev/null; then
    fail "independent validator includes a producer science/solver kernel"
fi

while IFS= read -r -d '' presentation_file; do
    if grep -Ein \
        '(^|[^A-Za-z])(pysam|cyvcf2|htslib|samtools|bcftools)([^A-Za-z]|$)' \
        "$presentation_file" >/dev/null; then
        fail "presentation code imports or invokes a genomics/science reader: $presentation_file"
    fi
done < <(find presentation -type f -name '*.py' -print0 2>/dev/null || true)

if ((failures > 0)); then
    echo "BOUNDARY RESULT: FAIL failures=${failures}" >&2
    exit 1
fi
echo "BOUNDARY RESULT: PASS validator_link=independent python_science=absent"
