# The pipeline (#43). On arm64 macOS, unit.pipeline also links and runs what it compiles.
add_executable(unit_pipeline pipeline_test.cpp)
target_link_libraries(unit_pipeline PRIVATE ycxxc_unit ycxxc_pipeline)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  add_test(NAME unit.pipeline COMMAND unit_pipeline --run ${CMAKE_CURRENT_BINARY_DIR}/run)
else()
  add_test(NAME unit.pipeline COMMAND unit_pipeline)
endif()

# The pipeline's cells (#63): what a second build and an edit re-execute, and incremental == cold.
ycxxc_unit_test(pipeline_incremental incremental_test.cpp ycxxc_pipeline)

ycxxc_unit_test(pipeline_elf_executable elf_executable_test.cpp ycxxc_pipeline)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME unit.linux_compile
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_compile.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_executable
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_executable.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_dynamic
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_dynamic.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_stdarg
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_stdarg.py $<TARGET_FILE:ycxxc>)
  add_test(NAME unit.pipeline_elf_run
    COMMAND ${CMAKE_COMMAND} -DPIPELINE=$<TARGET_FILE:unit_pipeline_elf_executable>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/elf_executable_run.cmake)
endif()
