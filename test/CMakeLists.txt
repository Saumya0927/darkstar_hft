include(GoogleTest)

# dhft_add_test(<name> [libs...]) — one executable per test file, auto-registered with CTest.
function(dhft_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

dhft_add_test(smoke_test       dhft)
dhft_add_test(check_test       dhft)       # Task 1
dhft_add_test(validate_test    dhft)       # Task 2
dhft_add_test(types_test       dhft)       # Task 1
dhft_add_test(events_test      dhft)       # Task 2
dhft_add_test(orderbook_test   dhft)       # Tasks 3-5
dhft_add_test(seams_test       dhft)       # Task 7
dhft_add_test(matching_test    dhft)       # Task 6
dhft_add_test(integration_test dhft_io)    # Task 8
dhft_add_test(golden_test      dhft_testkit)  # Task 3
dhft_add_test(generator_test   dhft_testkit)  # Task 4
dhft_add_test(property_test    dhft_testkit)  # Task 5
dhft_add_test(reference_test   dhft_reference)  # Task 6
dhft_add_test(equivalence_test dhft_testkit dhft_reference)  # Task 7
dhft_add_test(shrink_test      dhft_testkit)  # Task 8
dhft_add_test(bench_test       dhft_bench dhft_testkit)  # M3 Task 1
target_compile_definitions(equivalence_test PRIVATE
  DHFT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
target_compile_definitions(golden_test PRIVATE
  DHFT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
