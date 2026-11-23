# Process-level daemon tests (docs/design/daemon.md §8.2). #98 grows this suite.
add_test(NAME daemon.compare COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/compare.py $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/tests/own)
add_executable(ycxxc_daemon_process process_driver.cpp)
target_link_libraries(ycxxc_daemon_process PRIVATE ycxxc_lib ycxxc_new)
add_test(NAME daemon.process COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/process.py $<TARGET_FILE:ycxxc> $<TARGET_FILE:ycxxc_daemon_process>)
add_test(NAME daemon.freestanding
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/freestanding.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx)
add_test(NAME daemon.aggregate_alignment
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/aggregate_alignment.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.lsp COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/lsp.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace_tui COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace_tui.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.system_includes
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/system_includes.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.nostdinc
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/nostdinc.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/resources/linux/include)
