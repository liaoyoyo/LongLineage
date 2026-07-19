# SPDX-License-Identifier: GPL-3.0-only

option(
    LONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS
    "Register tests that require Git history or repository control files excluded from production build contexts"
    ON)

add_executable(test_dependency_smoke test_main.cpp)
target_link_libraries(test_dependency_smoke PRIVATE longlineage_core longlineage_build_options)
add_test(NAME dependency_smoke COMMAND test_dependency_smoke)
set_tests_properties(dependency_smoke PROPERTIES LABELS "unit;dependency;synthetic")

function(longlineage_add_standalone_test target source)
    add_executable("${target}" "${source}")
    target_link_libraries("${target}" PRIVATE longlineage_core longlineage_build_options)
endfunction()

longlineage_add_standalone_test(test_typed_io unit/test_typed_io.cpp)
add_test(NAME typed_io COMMAND test_typed_io)
add_test(NAME mm_ml_contract COMMAND test_typed_io)
add_test(NAME sidecar_join_contract COMMAND test_typed_io)
add_test(NAME preflight_contract COMMAND test_typed_io)
set_tests_properties(
    typed_io mm_ml_contract sidecar_join_contract preflight_contract
    PROPERTIES LABELS "unit;io;synthetic" RUN_SERIAL TRUE)

longlineage_add_standalone_test(test_runtime_solver unit/test_runtime_solver.cpp)
add_test(NAME runtime_solver COMMAND test_runtime_solver)
add_test(NAME determinism_contract COMMAND test_runtime_solver)
add_test(NAME topology_no_incomplete_winner COMMAND test_runtime_solver)
set_tests_properties(
    runtime_solver determinism_contract topology_no_incomplete_winner
    PROPERTIES LABELS "unit;runtime;solver;synthetic" RUN_SERIAL TRUE)

longlineage_add_standalone_test(test_block_pipeline unit/test_block_pipeline.cpp)
add_test(NAME p2_block_plan_boundaries COMMAND test_block_pipeline block-plan)
add_test(NAME p2_block_reader_indexed COMMAND test_block_pipeline block-reader)
add_test(NAME p2_reorder_sink_adversarial COMMAND test_block_pipeline reorder)
add_test(NAME p2_determinism_matrix COMMAND test_block_pipeline determinism)
add_test(NAME p2_worker_failure_cancellation COMMAND test_block_pipeline failure)
set_tests_properties(
    p2_block_plan_boundaries p2_block_reader_indexed p2_reorder_sink_adversarial
    p2_determinism_matrix p2_worker_failure_cancellation
    PROPERTIES LABELS "unit;integration;p2;synthetic" RUN_SERIAL TRUE TIMEOUT 120)

add_test(
    NAME cli_production_gates
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_cli_gates.sh"
            "$<TARGET_FILE:longlineage_producer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(
    NAME query_read_only_rejects_unfrozen
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/expect_exit.sh" 8
            "$<TARGET_FILE:longlineage_query>" --run-root
            "${CMAKE_CURRENT_BINARY_DIR}/missing-run" --artifact m1_sites)
set_tests_properties(
    cli_production_gates query_read_only_rejects_unfrozen
    PROPERTIES LABELS "integration;boundary;synthetic")

foreach(governance_check
        check-cold-start
        check-policy
        check-state
        check-catalog
        check-status-codes
        check-gates
        check-truth-boundary)
    string(REPLACE "-" "_" governance_test_name "${governance_check}")
    add_test(
        NAME "governance_${governance_test_name}"
        COMMAND "$<TARGET_FILE:longlineage_governance>" "${governance_check}"
                --repo "${CMAKE_CURRENT_SOURCE_DIR}/..")
    set_tests_properties(
        "governance_${governance_test_name}"
        PROPERTIES LABELS "governance;synthetic")
endforeach()

add_test(
    NAME governance_truth_manifest_negative
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/expect_exit.sh" 5
            "$<TARGET_FILE:longlineage_governance>" check-truth-boundary
            --repo "${CMAKE_CURRENT_SOURCE_DIR}/.."
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/invalid_production_boundary.json")
set_tests_properties(
    governance_truth_manifest_negative
    PROPERTIES LABELS "governance;negative;synthetic")

add_test(
    NAME ai_readiness_independent_governance_target
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ai/check_readiness.sh"
            "${CMAKE_BINARY_DIR}")
set_tests_properties(
    ai_readiness_independent_governance_target
    PROPERTIES LABELS "governance;ai;synthetic" RUN_SERIAL TRUE)

if(LONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS)
    add_test(
        NAME hygiene_untracked_forbidden_negative
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_hygiene_untracked.sh")
    set_tests_properties(
        hygiene_untracked_forbidden_negative
        PROPERTIES LABELS "governance;negative;synthetic;repository-context")
endif()

add_test(
    NAME governance_agent_task_unknown_field_negative
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_agent_task_negative.sh"
            "$<TARGET_FILE:longlineage_governance>"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(
    governance_agent_task_unknown_field_negative
    PROPERTIES LABELS "governance;negative;synthetic")

add_test(
    NAME governance_phase_state_negative
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_phase_state_negative.sh"
            "$<TARGET_FILE:longlineage_governance>"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(
    NAME governance_audit_evidence_negative
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_audit_evidence_negative.sh"
            "$<TARGET_FILE:longlineage_governance>"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(
    NAME gate_negative_binding_contract
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_gate_binding_negative.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ci/check_gate_test_coverage.sh"
            "${CMAKE_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(
    NAME release_attestation_run_gate
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_cli_gates.sh"
            "$<TARGET_FILE:longlineage_producer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(
    NAME json_schema_contract_fixtures
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ci/check_json_schema_fixtures.sh")
if(LONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS)
    add_test(
        NAME performance_benchmark_records
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ci/check_performance_benchmarks.sh")
    add_test(
        NAME dependency_lock_negative
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/testing/test_dependency_lock_negative.sh")
endif()
set(longlineage_governance_contract_tests
    governance_phase_state_negative governance_audit_evidence_negative gate_negative_binding_contract
    release_attestation_run_gate json_schema_contract_fixtures)
set_tests_properties(
    ${longlineage_governance_contract_tests}
    PROPERTIES LABELS "governance;contract;negative;synthetic")
if(LONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS)
    set_tests_properties(
        performance_benchmark_records dependency_lock_negative
        PROPERTIES LABELS "governance;contract;negative;synthetic;repository-context")
endif()

add_custom_target(
    check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/ci/check_all.sh"
            "${CMAKE_BINARY_DIR}"
    DEPENDS
        longlineage_producer
        longlineage_validator
        longlineage_query
        longlineage_legacy_exporter
        longlineage_evaluator
        longlineage_governance
        test_dependency_smoke
        test_typed_io
        test_runtime_solver
        test_block_pipeline
    USES_TERMINAL)
