# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.22)

project(
    LongLineage
    VERSION 0.1.0
    DESCRIPTION "Truth-isolated long-read co-occurrence and mutation-state family analysis"
    LANGUAGES CXX)

set(LONGLINEAGE_ZERO_GIT_COMMIT "0000000000000000000000000000000000000000")
set(LONGLINEAGE_GIT_COMMIT "" CACHE STRING
    "Source Git commit; verified against a clean checkout when Git metadata is available")
set(LONGLINEAGE_ALLOW_EXTERNAL_GIT_COMMIT_ASSERTION OFF CACHE BOOL
    "Trust an explicit source commit only when the .git-free builder verifies it externally")

execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE LONGLINEAGE_GIT_REV_PARSE_STATUS
    OUTPUT_VARIABLE LONGLINEAGE_DISCOVERED_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git status --porcelain=v1 --untracked-files=all
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE LONGLINEAGE_GIT_STATUS_STATUS
    OUTPUT_VARIABLE LONGLINEAGE_GIT_STATUS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(LENGTH "${LONGLINEAGE_DISCOVERED_GIT_COMMIT}" LONGLINEAGE_DISCOVERED_GIT_COMMIT_LENGTH)
set(LONGLINEAGE_GIT_CHECKOUT_AVAILABLE OFF)
if(LONGLINEAGE_GIT_REV_PARSE_STATUS EQUAL 0 AND
   LONGLINEAGE_DISCOVERED_GIT_COMMIT_LENGTH EQUAL 40 AND
   LONGLINEAGE_DISCOVERED_GIT_COMMIT MATCHES "^[0-9a-f]+$" AND
   NOT LONGLINEAGE_DISCOVERED_GIT_COMMIT STREQUAL "${LONGLINEAGE_ZERO_GIT_COMMIT}")
    set(LONGLINEAGE_GIT_CHECKOUT_AVAILABLE ON)
endif()

if("${LONGLINEAGE_GIT_COMMIT}" STREQUAL "")
    if(LONGLINEAGE_GIT_CHECKOUT_AVAILABLE AND
       LONGLINEAGE_GIT_STATUS_STATUS EQUAL 0 AND
       "${LONGLINEAGE_GIT_STATUS_OUTPUT}" STREQUAL "")
        set(LONGLINEAGE_GIT_COMMIT "${LONGLINEAGE_DISCOVERED_GIT_COMMIT}")
    else()
        set(LONGLINEAGE_GIT_COMMIT "${LONGLINEAGE_ZERO_GIT_COMMIT}")
        message(STATUS
            "Source commit provenance is unavailable or dirty; production executables will fail closed")
    endif()
else()
    string(LENGTH "${LONGLINEAGE_GIT_COMMIT}" LONGLINEAGE_GIT_COMMIT_LENGTH)
    if(NOT LONGLINEAGE_GIT_COMMIT_LENGTH EQUAL 40 OR
       NOT LONGLINEAGE_GIT_COMMIT MATCHES "^[0-9a-f]+$" OR
       "${LONGLINEAGE_GIT_COMMIT}" STREQUAL "${LONGLINEAGE_ZERO_GIT_COMMIT}")
        message(FATAL_ERROR
            "Explicit LONGLINEAGE_GIT_COMMIT must be a 40-character lowercase non-zero SHA-1")
    endif()
    if(LONGLINEAGE_GIT_CHECKOUT_AVAILABLE)
        if(NOT "${LONGLINEAGE_GIT_COMMIT}" STREQUAL "${LONGLINEAGE_DISCOVERED_GIT_COMMIT}")
            message(FATAL_ERROR
                "Explicit LONGLINEAGE_GIT_COMMIT does not match checkout HEAD")
        endif()
        if(NOT LONGLINEAGE_GIT_STATUS_STATUS EQUAL 0 OR
           NOT "${LONGLINEAGE_GIT_STATUS_OUTPUT}" STREQUAL "")
            message(FATAL_ERROR
                "Explicit LONGLINEAGE_GIT_COMMIT requires a clean tracked and untracked checkout")
        endif()
    elseif(NOT LONGLINEAGE_ALLOW_EXTERNAL_GIT_COMMIT_ASSERTION)
        message(FATAL_ERROR
            "A .git-free source commit assertion requires explicit trusted-builder opt-in")
    else()
        message(STATUS
            "Using an externally verified source commit assertion from a trusted .git-free builder")
    endif()
endif()
set(LONGLINEAGE_COMPILER_ID
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

include(GNUInstallDirs)
include(CTest)

option(LONGLINEAGE_BUILD_TESTS "Build synthetic-only tests" ON)
option(LONGLINEAGE_BUILD_BENCHMARKS "Build local, non-production benchmark executables" OFF)
option(LONGLINEAGE_REQUIRE_EXACT_HTSLIB "Require the production HTSlib 1.18 pin" ON)
option(LONGLINEAGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(LONGLINEAGE_SANITIZERS "" CACHE STRING "Comma-separated sanitizers (Debug only)")

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(cmake/CompilerPolicy.cmake)
include(cmake/Dependencies.cmake)

file(GLOB_RECURSE LONGLINEAGE_PRODUCER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/manifest/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/artifact/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cooccurrence/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/m1/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/m2/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/solver/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/science/*.cpp")

add_library(longlineage_core STATIC ${LONGLINEAGE_PRODUCER_SOURCES})
target_include_directories(longlineage_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(longlineage_core
    PUBLIC
        longlineage_build_options
        Boost::boost
        PkgConfig::HTSLIB
        PkgConfig::JANSSON
        OpenSSL::Crypto
        Threads::Threads)

add_library(longlineage_validation STATIC
    src/common/digest.cpp
    src/manifest/production_manifest.cpp
    src/validation/artifact_validator.cpp
    src/artifact/run_root.cpp)
target_include_directories(longlineage_validation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(longlineage_validation
    PUBLIC
        longlineage_build_options
        PkgConfig::HTSLIB
        PkgConfig::JANSSON
        OpenSSL::Crypto)

# Independent descriptive-regional validator. This trust-boundary target must
# never link longlineage_core or the compatibility producer/solver kernels.
add_library(longlineage_regional_validation STATIC
    src/validation/regional_compat_validator.cpp)
target_include_directories(longlineage_regional_validation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(longlineage_regional_validation
    PUBLIC
        longlineage_build_options
        PkgConfig::JANSSON
        OpenSSL::Crypto)

add_library(longlineage_hcc1395_audit STATIC
    src/audit/hcc1395_determinism.cpp)
target_include_directories(longlineage_hcc1395_audit
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(longlineage_hcc1395_audit
    PUBLIC
        longlineage_build_options
        PkgConfig::HTSLIB
        PkgConfig::JANSSON
        OpenSSL::Crypto)
target_compile_definitions(longlineage_hcc1395_audit
    PRIVATE
        LONGLINEAGE_GIT_COMMIT="${LONGLINEAGE_GIT_COMMIT}")

add_library(longlineage_cli_support STATIC
    apps/cli_support.cpp)
target_include_directories(longlineage_cli_support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/apps")
target_link_libraries(longlineage_cli_support
    PUBLIC
        longlineage_build_options
        PkgConfig::JANSSON
        OpenSSL::Crypto)
target_compile_definitions(longlineage_cli_support
    PUBLIC
        LONGLINEAGE_VERSION="${PROJECT_VERSION}"
        LONGLINEAGE_GIT_COMMIT="${LONGLINEAGE_GIT_COMMIT}"
        LONGLINEAGE_COMPILER_ID="${LONGLINEAGE_COMPILER_ID}")

function(longlineage_add_cli target_name output_name source_file)
    add_executable("${target_name}" "${source_file}")
    set_target_properties("${target_name}" PROPERTIES OUTPUT_NAME "${output_name}")
    target_link_libraries("${target_name}"
        PRIVATE longlineage_cli_support longlineage_build_options ${ARGN})
    install(TARGETS "${target_name}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

longlineage_add_cli(longlineage_producer longlineage apps/longlineage_main.cpp
    longlineage_core)

# Trust boundary: these targets intentionally do not link longlineage_core.
longlineage_add_cli(longlineage_validator longlineage-validate apps/validate_main.cpp
    longlineage_validation)
longlineage_add_cli(longlineage_audit longlineage-audit apps/audit_main.cpp
    longlineage_hcc1395_audit)
longlineage_add_cli(longlineage_query longlineage-query apps/query_main.cpp)
longlineage_add_cli(longlineage_legacy_exporter longlineage-export-legacy
    apps/export_legacy_main.cpp)
longlineage_add_cli(longlineage_evaluator longlineage-evaluate apps/evaluate_main.cpp)
longlineage_add_cli(longlineage_governance longlineage-governance
    apps/governance_main.cpp)
longlineage_add_cli(longlineage_regional_compat longlineage-regional-compat
    apps/regional_compat_main.cpp longlineage_core)
longlineage_add_cli(longlineage_regional_validator
    longlineage-regional-validate apps/regional_compat_validate_main.cpp
    longlineage_regional_validation)

add_custom_target(
    governance-check
    COMMAND "$<TARGET_FILE:longlineage_governance>" check-all
            --repo "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS longlineage_governance
    USES_TERMINAL)

if(LONGLINEAGE_BUILD_BENCHMARKS)
    add_executable(longlineage_benchmark_bgzf_writer
        benchmarks/bgzf_writer_microbenchmark.cpp)
    set_target_properties(longlineage_benchmark_bgzf_writer
        PROPERTIES OUTPUT_NAME "longlineage-benchmark-bgzf-writer")
    target_link_libraries(longlineage_benchmark_bgzf_writer
        PRIVATE longlineage_core longlineage_build_options)
endif()

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE ORIGIN.md DESTINATION "${CMAKE_INSTALL_DATADIR}/longlineage")

if(BUILD_TESTING AND LONGLINEAGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
