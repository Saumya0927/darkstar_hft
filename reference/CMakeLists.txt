# Brute-force reference implementation used as a test oracle. Deliberately unoptimised;
# never linked into the demo or any production target.
add_library(dhft_reference STATIC
  src/NaiveEngine.cpp
)
target_include_directories(dhft_reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft_reference PUBLIC dhft)
