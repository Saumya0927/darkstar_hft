add_library(dhft_bench STATIC
  src/Counters.cpp
  src/Harness.cpp
  src/Samples.cpp
)
target_include_directories(dhft_bench PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft_bench PUBLIC dhft)
