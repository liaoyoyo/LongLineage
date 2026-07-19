#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$repo_root/build}"
mode="${2:---allow-declared-blocked}"
registry="${3:-$repo_root/governance/gate_registry.tsv}"

[[ "$mode" == "--allow-declared-blocked" || "$mode" == "--strict" ]] || {
    echo "usage: check_gate_test_coverage.sh BUILD_DIR [--allow-declared-blocked|--strict] [REGISTRY]" >&2
    exit 2
}
[[ -f "$registry" ]] || {
    echo "GATE NEGATIVE BINDING FAIL: registry is not a regular file: $registry" >&2
    exit 1
}

missing=0
binding_errors=0
line_number=0
declare -A binding_owner=()
declare -A blocked_gate_reason=()

mark_gate_blocked() {
    local gate_id="$1"
    local reason="$2"
    if [[ -z "${blocked_gate_reason[$gate_id]+present}" ]]; then
        missing=$((missing + 1))
        blocked_gate_reason["$gate_id"]="$reason"
    else
        blocked_gate_reason["$gate_id"]+=";${reason}"
    fi
}

while IFS= read -r line || [[ -n "$line" ]]; do
    line_number=$((line_number + 1))
    mapfile -t fields < <(
        awk -F '\t' '{for (field = 1; field <= NF; ++field) print $field}' <<<"$line"
    )
    if [[ "${#fields[@]}" -ne 6 ]]; then
        echo "GATE NEGATIVE BINDING FAIL: line=${line_number} expected_fields=6 observed_fields=${#fields[@]}" >&2
        binding_errors=$((binding_errors + 1))
        continue
    fi

    gate_id="${fields[0]}"
    required="${fields[1]}"
    scope="${fields[2]}"
    command="${fields[3]}"
    negative_fixture="${fields[4]}"

    if [[ "$line_number" -eq 1 ]]; then
        expected_header=$'gate_id\trequired\tscope\tcommand\tnegative_fixture\tevidence_role'
        if [[ "$line" != "$expected_header" ]]; then
            echo "GATE NEGATIVE BINDING FAIL: registry header is not canonical" >&2
            binding_errors=$((binding_errors + 1))
        fi
        continue
    fi

    if [[ -z "$negative_fixture" ]]; then
        echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id:-<empty>} binding is missing" >&2
        binding_errors=$((binding_errors + 1))
    elif [[ -n "${binding_owner[$negative_fixture]+present}" ]]; then
        echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} duplicate_binding=${negative_fixture} first_gate=${binding_owner[$negative_fixture]}" >&2
        binding_errors=$((binding_errors + 1))
    else
        binding_owner["$negative_fixture"]="$gate_id"
        case "$negative_fixture" in
            ctest:*)
                test_id="${negative_fixture#ctest:}"
                if [[ ! "$test_id" =~ ^[A-Za-z0-9_.-]+$ ]]; then
                    echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} invalid_ctest_id=${test_id}" >&2
                    binding_errors=$((binding_errors + 1))
                else
                    listing="$(/usr/bin/ctest --test-dir "$build_dir" -N -R "^${test_id}$")"
                    listed_total="$(awk -F ': ' '/Total Tests:/{print $2}' <<<"$listing")"
                    listed_total="${listed_total:-0}"
                    exact_count="$(
                        awk -F ': ' -v expected="$test_id" '
                            /^  Test +#[0-9]+: / && $2 == expected {++count}
                            END {print count + 0}
                        ' <<<"$listing"
                    )"
                    if [[ "$listed_total" -ne 1 || "$exact_count" -ne 1 ]]; then
                        echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} ctest_id=${test_id} listed_total=${listed_total} exact_count=${exact_count}" >&2
                        binding_errors=$((binding_errors + 1))
                    else
                        echo "GATE NEGATIVE BINDING PASS: gate=${gate_id} kind=ctest target=${test_id}"
                    fi
                fi
                ;;
            fixture:*)
                fixture="${negative_fixture#fixture:}"
                fixture_root="$(readlink -f "$repo_root/tests/fixtures")"
                if [[ ! "$fixture" =~ ^tests/fixtures/[A-Za-z0-9._/-]+$ ||
                      "$fixture" == *"/../"* || "$fixture" == */.. ||
                      "$fixture" == *"//"* || -L "$repo_root/$fixture" ||
                      ! -s "$repo_root/$fixture" ]]; then
                    echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} fixture=${fixture} is absent, empty, unsafe, or not a regular in-repo fixture" >&2
                    binding_errors=$((binding_errors + 1))
                else
                    resolved_fixture="$(readlink -f "$repo_root/$fixture")"
                    if [[ "$resolved_fixture" != "$fixture_root/"* ||
                          ! -f "$resolved_fixture" ]]; then
                        echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} fixture=${fixture} escapes tests/fixtures" >&2
                        binding_errors=$((binding_errors + 1))
                    else
                        echo "GATE NEGATIVE BINDING PASS: gate=${gate_id} kind=fixture target=${fixture} execution_evidence=false"
                        if [[ "$required" == "true" ]]; then
                            mark_gate_blocked "$gate_id" "fixture_only"
                            echo "GATE NEGATIVE EXECUTION BLOCKED: gate=${gate_id} fixture=${fixture} reason=fixture_is_not_execution_evidence"
                        fi
                    fi
                fi
                ;;
            *)
                echo "GATE NEGATIVE BINDING FAIL: gate=${gate_id} unknown_binding=${negative_fixture}" >&2
                binding_errors=$((binding_errors + 1))
                ;;
        esac
    fi

    [[ "$required" != "true" ]] && continue
    if [[ "$command" =~ ^ctest[[:space:]]+-R[[:space:]]+([^[:space:]]+)$ ]]; then
        pattern="${BASH_REMATCH[1]}"
        listing="$(/usr/bin/ctest --test-dir "$build_dir" -N -R "$pattern")"
        count="$(awk -F ': ' '/Total Tests:/{print $2}' <<<"$listing")"
        count="${count:-0}"
        if [[ "$count" -eq 0 ]]; then
            echo "GATE TEST COVERAGE BLOCKED: gate=${gate_id} pattern=${pattern} phase_scope=${scope}"
            mark_gate_blocked "$gate_id" "missing_command_test:${pattern}"
        else
            echo "GATE TEST COVERAGE PASS: gate=${gate_id} pattern=${pattern} tests=${count}"
        fi
    fi
done <"$registry"

if [[ "$binding_errors" -ne 0 ]]; then
    echo "GATE NEGATIVE BINDING RESULT: FAIL errors=${binding_errors}" >&2
    exit 1
fi
echo "GATE NEGATIVE BINDING RESULT: PASS bindings=${#binding_owner[@]}"
for gate_id in "${!blocked_gate_reason[@]}"; do
    echo "GATE DECLARED BLOCKER: gate=${gate_id} reason=${blocked_gate_reason[$gate_id]}"
done | sort

if [[ "$mode" == "--strict" && "$missing" -ne 0 ]]; then
    echo "GATE TEST COVERAGE RESULT: FAIL missing=${missing}" >&2
    exit 1
fi
echo "GATE TEST COVERAGE RESULT: PASS_WITH_DECLARED_BLOCKERS missing=${missing} mode=${mode}"
