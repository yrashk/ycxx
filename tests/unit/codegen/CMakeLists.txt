# The back end (#26, #28): `lower` and AArch64 code generation. On arm64 macOS, unit.aarch64 also
# links what it generates with ycxx's linker and runs it, and links a C harness compiled by the
# bootstrap compiler as a black-box ABI oracle (DECISIONS §2.3).
add_library(ycxxc_unit_codegen STATIC)
target_sources(ycxxc_unit_codegen PUBLIC FILE_SET CXX_MODULES FILES layout.cppm)
target_link_libraries(ycxxc_unit_codegen PUBLIC ycxxc_unit ycxxc_lower)

ycxxc_unit_test(lower lower_test.cpp ycxxc_unit_codegen)

add_executable(unit_aarch64 aarch64_test.cpp)
target_link_libraries(unit_aarch64 PRIVATE ycxxc_unit_codegen ycxxc_aarch64 ycxxc_link)
# PARITY: by design: runs the arm64 Mach-O executables it links, which only macOS can
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  add_test(NAME unit.aarch64 COMMAND unit_aarch64 --run ${CMAKE_CURRENT_BINARY_DIR}/run --cc ${CMAKE_CXX_COMPILER})
else()
  add_test(NAME unit.aarch64 COMMAND unit_aarch64)
endif()

# PARITY: by design: runs x86-64 ELF executables, which only Linux can
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  ycxxc_unit_test(x86_64 x86_64_test.cpp ycxxc_x86_64 ycxxc_pipeline)
  target_sources(unit_x86_64 PRIVATE FILE_SET CXX_MODULES FILES x86_64_native.cppm)
endif()
