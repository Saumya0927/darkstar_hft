# darkstar_hft — limit order book + matching engine (Milestone 1)
cmake_minimum_required(VERSION 3.28)
project(darkstar_hft LANGUAGES CXX)

# C++26 for the whole project. Extensions off = standards-conforming.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options. Native tuning is opt-in because it produces a non-portable binary.
option(DHFT_ENABLE_LTO    "Enable ThinLTO (Release)"                 OFF)
option(DHFT_TUNE_NATIVE   "Tune for the host CPU (-mcpu=native)"     OFF)
option(DHFT_WERROR        "Treat warnings as errors"                 ON)

# Shared build settings live on one INTERFACE target that every other target links.
# Nothing is compiled into dhft_options itself; it just carries flags outward.
add_library(dhft_options INTERFACE)
target_compile_options(dhft_options INTERFACE -Wall -Wextra -Wpedantic)

if(DHFT_WERROR)
  target_compile_options(dhft_options INTERFACE -Werror)
endif()

# -mcpu=native, not -march=native: measured on arm64 Clang 22, -march=native detects
# markedly fewer host features than -mcpu=native.
if(DHFT_TUNE_NATIVE)
  target_compile_options(dhft_options INTERFACE -mcpu=native)
endif()

# ThinLTO is set directly rather than via CMAKE_INTERPROCEDURAL_OPTIMIZATION, which
# emits full -flto on Clang. -dead_strip is the Mach-O equivalent of --gc-sections and
# is not on by default; -ffunction-sections/-fdata-sections are no-ops on Mach-O.
if(DHFT_ENABLE_LTO)
  target_compile_options(dhft_options INTERFACE -flto=thin)
  target_link_options(dhft_options INTERFACE -flto=thin)
endif()
target_link_options(dhft_options INTERFACE $<$<CONFIG:Release>:-Wl,-dead_strip>)

# Debug builds run under AddressSanitizer + UndefinedBehaviorSanitizer.
# These catch use-after-free, invalidated iterators, and integer UB the moment they happen.
target_compile_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer;-g>)
target_link_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# The core engine library: order book + matching engine. No file or stream I/O.
add_library(dhft STATIC
  src/OrderBook.cpp
  src/MatchingEngine.cpp
)
target_include_directories(dhft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft PUBLIC dhft_options)

# Text I/O module (script parsing, human-readable output), built on the core engine.
add_subdirectory(io)

# Test-only support library (golden harness, generators).
add_subdirectory(testkit)

# GoogleTest, fetched at configure time (no manual checkout).
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(test)
add_subdirectory(apps)
