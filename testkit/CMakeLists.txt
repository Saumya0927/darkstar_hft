# Test-only support: golden-file harness and (later) the random script generator.
add_library(dhft_testkit STATIC
  src/Generate.cpp
  src/Golden.cpp
)
target_include_directories(dhft_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft_testkit PUBLIC dhft_io dhft_reference)
