# On arm64 macOS the test also writes, signs, checks and runs executables, and links an object
# compiled by the bootstrap compiler as a black-box input (DECISIONS §2.3).
add_executable(unit_link link_test.cpp)
target_link_libraries(unit_link PRIVATE ycxxc_unit ycxxc_link)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  add_test(NAME unit.link COMMAND unit_link --run ${CMAKE_CURRENT_BINARY_DIR}/run --cc ${CMAKE_CXX_COMPILER})
else()
  add_test(NAME unit.link COMMAND unit_link)
endif()
