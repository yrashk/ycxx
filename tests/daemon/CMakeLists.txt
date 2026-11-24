# Process-level daemon tests (docs/design/daemon.md §8.2). #98 grows this suite.
add_test(NAME daemon.runtime_dir
  COMMAND python3 -B -X utf8 ${CMAKE_CURRENT_SOURCE_DIR}/runtime_dir_test.py)
add_test(NAME daemon.auto_var_init
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/auto_var_init.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.compare COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/compare.py $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/tests/own)
# Overlap the comparison with other tests even before CTest has timing history (#1230).
set_tests_properties(daemon.compare PROPERTIES COST 400)
add_test(NAME daemon.no_rtti
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/no_rtti.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx "${CMAKE_CXX_COMPILER}")
add_executable(ycxxc_daemon_process process_driver.cpp)
target_link_libraries(ycxxc_daemon_process PRIVATE ycxxc_lib ycxxc_new)
add_test(NAME daemon.process COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/process.py $<TARGET_FILE:ycxxc> $<TARGET_FILE:ycxxc_daemon_process>)
add_test(NAME daemon.freestanding
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/freestanding.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx)
add_test(NAME daemon.aggregate_alignment
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/aggregate_alignment.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.pic
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/pic.py $<TARGET_FILE:ycxxc>
          ${PROJECT_SOURCE_DIR}/deps/libycxx "${CMAKE_CXX_COMPILER}" "${CMAKE_LINKER}")
add_test(NAME daemon.lsp COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/lsp.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.trace_tui COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/trace_tui.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.system_includes
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/system_includes.py $<TARGET_FILE:ycxxc>)
add_test(NAME daemon.nostdincxx
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/nostdincxx.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx)
add_test(NAME daemon.nostdinc
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/nostdinc.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/resources/linux/include)
if(CMAKE_GENERATOR MATCHES "^Ninja")
  set(YCXX_DEPFILE_TEST_NINJA "${CMAKE_MAKE_PROGRAM}")
else()
  find_program(YCXX_DEPFILE_TEST_NINJA NAMES ninja ninja-build)
endif()
add_test(NAME daemon.depfiles
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/depfiles.py
          $<TARGET_FILE:ycxxc> "${YCXX_DEPFILE_TEST_NINJA}")
add_test(NAME daemon.no_exceptions
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/no_exceptions.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx "${CMAKE_READELF}")

# PARITY: by design: checks x86-64 ELF relocation metadata and runs native mixed objects.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME daemon.weak_undefined_object
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/weak_undefined_object.py
            $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/deps/libycxx
            "${CMAKE_CXX_COMPILER}" "${CMAKE_READELF}" "${CMAKE_AR}")
  add_test(NAME daemon.derived_virtual_destructor
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/derived_virtual_destructor.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME daemon.derived_virtual_destructor_alignment
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/derived_virtual_destructor_alignment.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
endif()

add_test(NAME daemon.warning_options
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/warning_options.py $<TARGET_FILE:ycxxc>)

add_test(NAME daemon.engine_snapshot
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/engine_snapshot.py
          $<TARGET_FILE:ycxxc> $<TARGET_FILE:unit_engine_snapshot>)

add_test(NAME daemon.project_build COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/project_build.py $<TARGET_FILE:ycxxc>)

add_test(NAME daemon.project_build_watch COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/project_build_watch.py $<TARGET_FILE:ycxxc>)

add_test(NAME daemon.project_inspection COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/project_inspection.py $<TARGET_FILE:ycxxc>)
