# The engine trace (#245): the stream against a golden file, and the browser's model.
add_executable(unit_trace trace_test.cpp)
target_link_libraries(unit_trace PRIVATE ycxxc_unit ycxxc_pipeline ycxxc_trace)
add_test(NAME unit.trace COMMAND unit_trace ${CMAKE_CURRENT_SOURCE_DIR}/stream.golden)
