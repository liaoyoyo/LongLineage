# SPDX-License-Identifier: GPL-3.0-only

find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

pkg_check_modules(HTSLIB REQUIRED IMPORTED_TARGET htslib)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

set(LONGLINEAGE_PINNED_HTSLIB_VERSION "1.18")
if(LONGLINEAGE_REQUIRE_EXACT_HTSLIB AND
   NOT HTSLIB_VERSION VERSION_EQUAL LONGLINEAGE_PINNED_HTSLIB_VERSION)
    message(FATAL_ERROR
        "Production requires HTSlib ${LONGLINEAGE_PINNED_HTSLIB_VERSION}; "
        "pkg-config resolved ${HTSLIB_VERSION}")
endif()

if(HTSLIB_VERSION VERSION_LESS "1.18")
    message(FATAL_ERROR "HTSlib >= 1.18 is required")
endif()

if(JANSSON_VERSION VERSION_LESS "2.13")
    message(FATAL_ERROR "Jansson >= 2.13 is required")
endif()

message(STATUS "LongLineage dependency lock:")
message(STATUS "  HTSlib: ${HTSLIB_VERSION} (production pin: ${LONGLINEAGE_PINNED_HTSLIB_VERSION})")
message(STATUS "  Jansson: ${JANSSON_VERSION}")
message(STATUS "  OpenSSL: ${OPENSSL_VERSION}")
message(STATUS "  Threads: ${CMAKE_THREAD_LIBS_INIT}")
