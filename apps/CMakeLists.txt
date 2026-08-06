add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE dhft_io)

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE dhft_bench dhft_testkit)
