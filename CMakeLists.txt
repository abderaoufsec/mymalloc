cmake_minimum_required(VERSION 3.20)

project(mymalloc
        VERSION 0.1.0
        DESCRIPTION "Educational user-space memory allocator"
        HOMEPAGE_URL "https://github.com/abderaoufsec/mymalloc"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MYMALLOC_BUILD_TESTS "Build the mymalloc test suite" ON)
option(MYMALLOC_BUILD_EXAMPLES "Build the mymalloc examples" ON)
option(MYMALLOC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(MYMALLOC_SANITIZER "off" CACHE STRING
    "Enable sanitizer(s): off | address | undefined | address-undefined")
set_property(CACHE MYMALLOC_SANITIZER PROPERTY STRINGS off address undefined address-undefined)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Debug when no build type is given (single-config generators).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MymallocWarnings)
include(MymallocSanitizers)

mymalloc_apply_warnings()
mymalloc_apply_sanitizers()

# ---------------------------------------------------------------------------
# Library target
#
# Phase 0 only ships version metadata; my_malloc/my_free/my_calloc/my_realloc
# are declared in <mymalloc/mymalloc.h> but implemented in later phases.
# ---------------------------------------------------------------------------
add_library(mymalloc src/version.cpp src/raw_memory.cpp src/alignment.cpp)
add_library(mymalloc::mymalloc ALIAS mymalloc)

target_include_directories(mymalloc
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/mymalloc/config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/mymalloc/config.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# Tests and examples
# ---------------------------------------------------------------------------
if(MYMALLOC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MYMALLOC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()