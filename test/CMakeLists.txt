include(GoogleTest)

# dhft_add_test(<name> [libs...]) — one executable per test file, auto-registered with CTest.
function(dhft_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

dhft_add_test(smoke_test       dhft)
dhft_add_test(types_test       dhft)       # Task 1
dhft_add_test(events_test      dhft)       # Task 2
dhft_add_test(orderbook_test   dhft)       # Tasks 3-5
dhft_add_test(seams_test       dhft)       # Task 7
dhft_add_test(matching_test    dhft)       # Task 6
dhft_add_test(integration_test dhft_io)    # Task 8
