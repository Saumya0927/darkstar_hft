# darkstar_hft — limit order book + matching engine (Milestone 1)
cmake_minimum_required(VERSION 3.28)
project(darkstar_hft LANGUAGES CXX)

# C++26 for the whole project. Extensions off = standards-conforming.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared build settings live on one INTERFACE target that every other target links.
# Nothing is compiled into dhft_options itself; it just carries flags outward.
add_library(dhft_options INTERFACE)
target_compile_options(dhft_options INTERFACE -Wall -Wextra -Wpedantic)

# Debug builds run under AddressSanitizer + UndefinedBehaviorSanitizer.
# These catch use-after-free, invalidated iterators, and integer UB the moment they happen.
target_compile_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer;-g>)
target_link_options(dhft_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# The library. Header-only for now (Types.h/Events.h). Starting Task 3, when the first
# .cpp appears in src/, change this to:
#     add_library(dhft STATIC src/OrderBook.cpp src/MatchingEngine.cpp src/Feed.cpp src/Sink.cpp)
#     target_link_libraries(dhft PUBLIC dhft_options)
# and keep the include-dir line below.
add_library(dhft INTERFACE)
target_include_directories(dhft INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft INTERFACE dhft_options)

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

# apps/ (the demo) is added in Task 8:
#   add_subdirectory(apps)
