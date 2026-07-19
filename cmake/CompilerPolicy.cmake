# SPDX-License-Identifier: GPL-3.0-only

set(_longlineage_flag_variables
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_CXX_FLAGS_MINSIZEREL)

foreach(_flag_variable IN LISTS _longlineage_flag_variables)
    string(TOLOWER "${${_flag_variable}}" _flag_value)
    if(_flag_value MATCHES "(^|[ \t])-ffast-math([ \t]|$)")
        message(FATAL_ERROR "${_flag_variable} contains forbidden -ffast-math")
    endif()
    if(_flag_value MATCHES "(^|[ \t])-march=native([ \t]|$)")
        message(FATAL_ERROR "${_flag_variable} contains forbidden -march=native")
    endif()
    if(_flag_value MATCHES "(^|[ \t])-ofast([ \t]|$)")
        message(FATAL_ERROR "${_flag_variable} contains forbidden -Ofast semantics")
    endif()
endforeach()

add_library(longlineage_build_options INTERFACE)
target_compile_features(longlineage_build_options INTERFACE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(longlineage_build_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
        -Wundef
        -Werror=return-type
        -fno-fast-math
        -ffp-contract=off)
    if(LONGLINEAGE_WARNINGS_AS_ERRORS)
        target_compile_options(longlineage_build_options INTERFACE -Werror)
    endif()
else()
    message(FATAL_ERROR
        "The v1 production policy currently supports only GCC and Clang; "
        "resolved compiler is ${CMAKE_CXX_COMPILER_ID}")
endif()

if(LONGLINEAGE_SANITIZERS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR "Sanitizers are supported only with Debug builds")
    endif()
    string(REPLACE "," ";" _sanitizer_list "${LONGLINEAGE_SANITIZERS}")
    list(JOIN _sanitizer_list "," _sanitizer_flags)
    target_compile_options(longlineage_build_options INTERFACE
        "-fsanitize=${_sanitizer_flags}"
        -fno-omit-frame-pointer)
    target_link_options(longlineage_build_options INTERFACE
        "-fsanitize=${_sanitizer_flags}"
        -fno-omit-frame-pointer)
endif()
