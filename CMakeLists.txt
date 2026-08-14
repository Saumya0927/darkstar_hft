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
option(DHFT_WHOLE_PROGRAM_VTABLES "Devirtualize across the LTO unit" OFF)

set(DHFT_PGO "off" CACHE STRING "Profile-guided optimization: off, generate or use")
set_property(CACHE DHFT_PGO PROPERTY STRINGS off generate use)
set(DHFT_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata, required when DHFT_PGO=use")

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

# T10 experiment: let the linker resolve virtual calls it can prove have one target.
# Needs LTO to see across translation units; asserting otherwise would silently do nothing.
if(DHFT_WHOLE_PROGRAM_VTABLES)
  if(NOT DHFT_ENABLE_LTO)
    message(FATAL_ERROR "DHFT_WHOLE_PROGRAM_VTABLES needs DHFT_ENABLE_LTO=ON")
  endif()
  # The hidden-visibility flags are not optional extras. Measured: -fwhole-program-vtables
  # on its own produces a byte-identical binary under Apple ld64, because with default
  # visibility the compiler must assume a class could be overridden outside the link unit.
  # With hidden visibility the three indirect calls in MatchingEngine::process become zero.
  target_compile_options(dhft_options INTERFACE
    -fwhole-program-vtables -fvisibility=hidden -fvisibility-inlines-hidden)
  target_link_options(dhft_options INTERFACE -fwhole-program-vtables)
endif()

# T10 experiment: instrumentation PGO. Build with generate, run the benchmark, merge the
# raw profiles with llvm-profdata, then rebuild with use pointing at the merged file.
if(DHFT_PGO STREQUAL "generate")
  target_compile_options(dhft_options INTERFACE -fprofile-generate)
  target_link_options(dhft_options INTERFACE -fprofile-generate)
elseif(DHFT_PGO STREQUAL "use")
  if(NOT EXISTS "${DHFT_PGO_PROFILE}")
    message(FATAL_ERROR "DHFT_PGO=use needs DHFT_PGO_PROFILE set to a merged .profdata file")
  endif()
  # A function the profile never saw is a warning, and -Werror would turn the whole
  # experiment into a build failure rather than a measurement. backend-plugin covers the
  # hash mismatch every other executable's main() gets: profiles key on function name, so
  # demo's main collides with the bench main the profile was trained on. Its count is
  # discarded, which is the correct outcome -- only apps/bench.cpp is being measured.
  target_compile_options(dhft_options INTERFACE
    -fprofile-use=${DHFT_PGO_PROFILE}
    -Wno-error=profile-instr-unprofiled
    -Wno-error=profile-instr-out-of-date
    -Wno-error=backend-plugin)
elseif(NOT DHFT_PGO STREQUAL "off")
  message(FATAL_ERROR "DHFT_PGO must be off, generate or use (got '${DHFT_PGO}')")
endif()
target_link_options(dhft_options INTERFACE $<$<CONFIG:Release>:-Wl,-dead_strip>)

# Debug builds run under AddressSanitizer + UndefinedBehaviorSanitizer.
# These catch use-after-free, invalidated iterators, and integer UB the moment they happen.
target_compile_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer;-g>)
target_link_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# Open-addressed hash map for the order-id index. Pinned, not tracking main.
include(FetchContent)
FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG        v4.9.0
)
FetchContent_MakeAvailable(unordered_dense)

# The core engine library: order book + matching engine. No file or stream I/O.
add_library(dhft STATIC
  src/OrderBook.cpp
  src/MatchingEngine.cpp
)
target_include_directories(dhft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft PUBLIC dhft_options unordered_dense::unordered_dense)

# Text I/O module (script parsing, human-readable output), built on the core engine.
add_subdirectory(io)

# Test-only: brute-force oracle and the harness/generator support library.
add_subdirectory(reference)
add_subdirectory(testkit)

# Measurement harness (Milestone 3).
add_subdirectory(bench)

# GoogleTest, fetched at configure time (no manual checkout).
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(test)
add_subdirectory(apps)
