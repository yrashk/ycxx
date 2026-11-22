# On arm64 macOS the test also writes, signs, checks and runs executables, and links an object
# compiled by the bootstrap compiler as a black-box input (DECISIONS §2.3).
add_executable(unit_link link_test.cpp)
target_link_libraries(unit_link PRIVATE ycxxc_unit ycxxc_link)
# PARITY: by design: runs the arm64 Mach-O executables it links, which only macOS can
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  add_test(NAME unit.link COMMAND unit_link --run ${CMAKE_CURRENT_BINARY_DIR}/run --cc ${CMAKE_CXX_COMPILER})
else()
  add_test(NAME unit.link COMMAND unit_link)
endif()

ycxxc_unit_test(elf_link elf_test.cpp ycxxc_link)
ycxxc_unit_test(elf_shared elf_shared_test.cpp ycxxc_link)
# PARITY: by design: runs x86-64 ELF executables, which only Linux can
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  find_program(YCXXC_READELF NAMES readelf llvm-readelf)
  add_test(NAME unit.elf_link_exec
    COMMAND ${CMAKE_COMMAND} -DLINKER=$<TARGET_FILE:unit_elf_link> -DREADELF=${YCXXC_READELF}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/elf_exec.cmake)
endif()
