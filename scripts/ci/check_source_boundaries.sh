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
    if ! python3 - "$presentation_file" <<'PY'
import ast
import sys

path = sys.argv[1]
tree = ast.parse(open(path, encoding="utf-8").read(), filename=path)
forbidden_modules = {"pysam", "cyvcf2", "htslib", "vcf"}
forbidden_commands = {"samtools", "bcftools"}

for node in ast.walk(tree):
    if isinstance(node, ast.Import):
        modules = {alias.name.split(".", 1)[0] for alias in node.names}
        if modules & forbidden_modules:
            raise SystemExit(1)
    if isinstance(node, ast.ImportFrom):
        module = (node.module or "").split(".", 1)[0]
        if module in forbidden_modules:
            raise SystemExit(1)
    if not isinstance(node, ast.Call):
        continue
    function = node.func
    is_process_call = (
        isinstance(function, ast.Name) and function.id in {"system", "popen"}
    ) or (
        isinstance(function, ast.Attribute)
        and function.attr in {"run", "call", "check_call", "check_output", "Popen", "system", "popen"}
    )
    if not is_process_call:
        continue
    for argument in node.args:
        if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
            first = argument.value.strip().split(maxsplit=1)[0].rsplit("/", 1)[-1]
            if first in forbidden_commands:
                raise SystemExit(1)
        if isinstance(argument, (ast.List, ast.Tuple)) and argument.elts:
            first_node = argument.elts[0]
            if isinstance(first_node, ast.Constant) and isinstance(first_node.value, str):
                first = first_node.value.rsplit("/", 1)[-1]
                if first in forbidden_commands:
                    raise SystemExit(1)
PY
    then
        fail "presentation code imports or invokes a genomics/science reader: $presentation_file"
    fi
done < <(find presentation -type f -name '*.py' -print0 2>/dev/null || true)

if ((failures > 0)); then
    echo "BOUNDARY RESULT: FAIL failures=${failures}" >&2
    exit 1
fi
echo "BOUNDARY RESULT: PASS validator_link=independent python_science=absent"
