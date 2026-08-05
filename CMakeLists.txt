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

# The compiled library. Add each new .cpp here as the milestone progresses
# (MatchingEngine.cpp, Feed.cpp, Sink.cpp in later tasks).
add_library(dhft STATIC
  src/OrderBook.cpp
  src/MatchingEngine.cpp
  src/Script.cpp
  src/Sink.cpp
)
target_include_directories(dhft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft PUBLIC dhft_options)

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
