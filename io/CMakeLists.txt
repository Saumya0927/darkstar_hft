# Text I/O for the engine: order-script parsing and human-readable event output.
# Separate from the core engine library so `dhft` stays free of file/stream concerns.
add_library(dhft_io STATIC
  src/Script.cpp
  src/TextSink.cpp
)
target_include_directories(dhft_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dhft_io PUBLIC dhft)
