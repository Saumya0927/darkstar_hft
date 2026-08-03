include(GoogleTest)

function(dhft_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE dhft GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

dhft_add_test(smoke_test)
dhft_add_test(types_test)            # Task 1
dhft_add_test(events_test)           # Task 2
# Added as the milestone progresses:
#   dhft_add_test(orderbook_test)    # Tasks 3-5
#   dhft_add_test(matching_test)     # Task 6
#   dhft_add_test(integration_test)  # Task 8
