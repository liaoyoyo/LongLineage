# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.22)

project(
    LongLineage
    VERSION 0.1.0
    DESCRIPTION "Truth-isolated long-read co-occurrence and mutation-state family analysis"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

option(LONGLINEAGE_BUILD_TESTS "Build synthetic-only tests" ON)
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/manifest/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/artifact/*.cpp"
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
        PkgConfig::HTSLIB
        PkgConfig::JANSSON
        OpenSSL::Crypto
        Threads::Threads)

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
    PUBLIC LONGLINEAGE_VERSION="${PROJECT_VERSION}")

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
longlineage_add_cli(longlineage_validator longlineage-validate apps/validate_main.cpp)
longlineage_add_cli(longlineage_query longlineage-query apps/query_main.cpp)
longlineage_add_cli(longlineage_legacy_exporter longlineage-export-legacy
    apps/export_legacy_main.cpp)
longlineage_add_cli(longlineage_evaluator longlineage-evaluate apps/evaluate_main.cpp)
longlineage_add_cli(longlineage_governance longlineage-governance
    apps/governance_main.cpp)

add_custom_target(
    governance-check
    COMMAND "$<TARGET_FILE:longlineage_governance>" check-all
            --repo "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS longlineage_governance
    USES_TERMINAL)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE ORIGIN.md DESTINATION "${CMAKE_INSTALL_DATADIR}/longlineage")

if(BUILD_TESTING AND LONGLINEAGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
