#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

failures=0
warnings=0

fail() {
    echo "READINESS FAIL: $*" >&2
    failures=$((failures + 1))
}

warn() {
    echo "READINESS WARN: $*" >&2
    warnings=$((warnings + 1))
}

pass() {
    echo "READINESS PASS: $*"
}

required=(
    AGENTS.md
    README.md
    ROADMAP.md
    LICENSE
    .ai/README.md
    .ai/templates/task.json
    docs/CURRENT_FOCUS.md
    docs/development/AI_WORKFLOW.md
    docs/development/WORKFLOW.md
    docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md
    docs/data/DATA_CONTRACTS.md
    docs/data/QUERY_GUIDE.md
    docs/release/RELEASE_GATES.md
    governance/agent_task.schema.json
    governance/audit_evidence.schema.json
    governance/gate_registry.tsv
    governance/phase_ledger.schema.json
    governance/project_state.schema.json
    governance/repository_policy.json
    schema/catalog.json
    schema/id_registry.json
    schema/core/contract_registry_bindings.schema.json
    schema/core/source_to_target_manifest.schema.json
    contracts/v1/artifact_roles.tsv
    contracts/v1/lifecycle_codes.tsv
    contracts/v1/query_operators.tsv
    contracts/v1/status_reason_codes.tsv
    contracts/v1/transform_registry.tsv
    contracts/v1/type_registry.tsv
    schema/core/production_input_authority.schema.json
    schema/core/release_attestation.schema.json
    oracle/production_input_authority.json
    state/project_state.json
    state/phase_ledger.json
    state/release_attestation.json
)
for path in "${required[@]}"; do
    if [[ ! -f "$path" ]]; then
        fail "missing source of truth: LongLineage/$path"
    fi
done

for json_file in governance/repository_policy.json schema/catalog.json \
                 state/project_state.json state/phase_ledger.json \
                 oracle/authority_manifest.json \
                 oracle/production_input_authority.json \
                 schema/core/production_input_authority.schema.json \
                 schema/core/contract_registry_bindings.schema.json \
                 schema/core/source_to_target_manifest.schema.json \
                 schema/core/release_attestation.schema.json \
                 governance/audit_evidence.schema.json \
                 governance/project_state.schema.json \
                 governance/phase_ledger.schema.json \
                 state/release_attestation.json \
                 provenance/source_to_target_manifest.json; do
    if ! jq -e . "$json_file" >/dev/null; then
        fail "invalid JSON: LongLineage/$json_file"
    fi
done

cmake_version="$(/usr/bin/cmake --version | awk 'NR==1 {print $3}')"
cmake_major="${cmake_version%%.*}"
cmake_tail="${cmake_version#*.}"
cmake_minor="${cmake_tail%%.*}"
if ((cmake_major < 3 || (cmake_major == 3 && cmake_minor < 22))); then
    fail "/usr/bin/cmake is ${cmake_version}; >=3.22 is required"
else
    pass "/usr/bin/cmake=${cmake_version}"
fi

for tool in c++ pkg-config jq sha256sum /usr/bin/jsonschema; do
    if ! command -v "$tool" >/dev/null; then
        fail "required development tool is absent: $tool"
    fi
done

htslib_version="$(pkg-config --modversion htslib 2>/dev/null || true)"
if [[ "$htslib_version" != "1.18" ]]; then
    fail "HTSlib exact pin mismatch: expected 1.18, observed ${htslib_version:-ABSENT}"
else
    pass "HTSlib=1.18"
fi
for module in jansson openssl; do
    if ! pkg-config --exists "$module"; then
        fail "pkg-config dependency absent: $module"
    else
        pass "$module=$(pkg-config --modversion "$module")"
    fi
done

requested_build_dir="${1:-}"
governance_binary=""
if [[ -n "$requested_build_dir" ]]; then
    candidate="${requested_build_dir%/}/bin/longlineage-governance"
    if [[ -x "$candidate" ]]; then
        governance_binary="$candidate"
    else
        fail "requested governance binary is absent or not executable: $candidate"
    fi
else
    for candidate in build*/bin/longlineage-governance; do
        if [[ -x "$candidate" ]] &&
           { [[ -z "$governance_binary" ]] || [[ "$candidate" -nt "$governance_binary" ]]; }; then
            governance_binary="$candidate"
        fi
    done
fi
if [[ -n "$governance_binary" ]]; then
    stale_source=""
    # longlineage-governance is an intentional trust-boundary executable: it
    # links cli_support but not longlineage_core. Producer headers/sources must
    # therefore not make this independent binary appear stale. CMakeLists is
    # included so any future target/source linkage change forces a rebuild.
    governance_build_inputs=(
        CMakeLists.txt
        cmake/CompilerPolicy.cmake
        cmake/Dependencies.cmake
        apps/cli_support.cpp
        apps/cli_support.hpp
        apps/governance_main.cpp
    )
    for source in "${governance_build_inputs[@]}"; do
        if [[ "$source" -nt "$governance_binary" ]]; then
            stale_source="$source"
            break
        fi
    done
    if [[ -n "$stale_source" ]]; then
        fail "compiled governance binary is stale; newer source: LongLineage/$stale_source"
    elif ! "$governance_binary" check-all --repo "$repo_root"; then
        fail "compiled governance checks rejected the repository"
    else
        pass "compiled governance checks via LongLineage/$governance_binary"
    fi
else
    warn "compiled governance binary absent; configure/build before relying on full policy checks"
fi

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [[ -n "$(git status --short)" ]]; then
        warn "worktree is dirty; preserve and review every pre-existing change"
    else
        pass "git worktree clean"
    fi
else
    warn "git repository not initialized yet; remote/privacy checks remain open"
fi

if ((failures > 0)); then
    echo "READINESS RESULT: FAIL failures=${failures} warnings=${warnings}" >&2
    exit 1
fi
echo "READINESS RESULT: PASS failures=0 warnings=${warnings}"
