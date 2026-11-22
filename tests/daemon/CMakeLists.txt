# Process-level daemon tests (docs/design/daemon.md §8.2). #98 grows this suite.
add_test(NAME daemon.compare COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/compare.py $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/tests/own)
add_test(NAME daemon.process COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/process.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.lsp COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/lsp.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace_tui COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace_tui.py $<TARGET_FILE:ycxxc>)
