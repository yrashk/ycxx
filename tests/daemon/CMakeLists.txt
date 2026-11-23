# Process-level daemon tests (docs/design/daemon.md §8.2). #98 grows this suite.
add_test(NAME daemon.compare COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/compare.py $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/tests/own)
add_executable(ycxxc_daemon_process process_driver.cpp)
target_link_libraries(ycxxc_daemon_process PRIVATE ycxxc_lib ycxxc_new)
add_test(NAME daemon.process COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/process.py $<TARGET_FILE:ycxxc> $<TARGET_FILE:ycxxc_daemon_process>)
add_test(NAME daemon.freestanding
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/freestanding.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx)
add_test(NAME daemon.lsp COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/lsp.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace_tui COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace_tui.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.system_includes
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/system_includes.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.nostdinc
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/nostdinc.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/resources/linux/include)

# PARITY: by design: checks x86-64 ELF relocation metadata and runs native mixed objects.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME daemon.derived_virtual_destructor
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/derived_virtual_destructor.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME daemon.derived_virtual_destructor_alignment
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/derived_virtual_destructor_alignment.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
endif()
