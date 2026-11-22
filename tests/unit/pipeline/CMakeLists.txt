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
add_test(NAME unit.dependent_fabsl
  COMMAND ${CMAKE_COMMAND} -DYCXX=$<TARGET_FILE:ycxxc>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/dependent_fabsl.cmake)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND NOT CMAKE_CROSSCOMPILING)
  # The Mach-O counterpart of unit.linux_inline (#124).
  find_program(YCXXC_NM NAMES nm REQUIRED)
  add_test(NAME unit.darwin_inline
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/darwin_inline.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${YCXXC_NM} ${CMAKE_AR})
  add_test(NAME unit.tu_local_parameter_symbols
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tu_local_parameter_symbols.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} darwin)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME unit.linux_compile
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_compile.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF} ${CMAKE_AR}
            ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_array_delete
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_array_delete.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    add_test(NAME unit.linux_fixed_array_new
      COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_fixed_array_new.py
              $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  endif()
  add_test(NAME unit.linux_variadic_definitions
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_variadic_definitions.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_defaulted_copy
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_defaulted_copy.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
  add_test(NAME unit.linux_scalar_sse
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_scalar_sse.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_virtual_consumer
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_virtual_consumer.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_function_addresses
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_function_addresses.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.tu_local_parameter_symbols
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tu_local_parameter_symbols.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} linux ${CMAKE_READELF})
  add_test(NAME unit.linux_executable
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_executable.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_cleanup_free_unwind
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_cleanup_free_unwind.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF} ${CMAKE_OBJDUMP})
  add_test(NAME unit.linux_native_int_throw
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_native_int_throw.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_executable(unit_linux_hosted_catch_all_object linux_hosted_catch_all_object.cpp)
  target_link_libraries(unit_linux_hosted_catch_all_object PRIVATE ycxxc_x86_64 ycxxc_obj)
  add_test(NAME unit.linux_hosted_catch_all_object
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_hosted_catch_all.py
            $<TARGET_FILE:unit_linux_hosted_catch_all_object> $<TARGET_FILE:ycxxc>
            ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_namespace_lambda
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_namespace_lambda.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_archive
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_archive.py
            $<TARGET_FILE:ycxxc> ${CMAKE_AR})
  add_test(NAME unit.linux_archive_output
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_archive_output.py
            $<TARGET_FILE:ycxxc> ${CMAKE_AR} ${CMAKE_NM} ${CMAKE_LINKER} ${CMAKE_C_COMPILER})
  add_test(NAME unit.linux_inline
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_inline.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF} ${CMAKE_AR})
  add_test(NAME unit.linux_elf_comdat
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_elf_comdat.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_AR})
  add_test(NAME unit.linux_std_mangling
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_std_mangling.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF}
            ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_dynamic
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_dynamic.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_stdarg
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_stdarg.py $<TARGET_FILE:ycxxc>)
  add_test(NAME unit.pipeline_elf_run
    COMMAND ${CMAKE_COMMAND} -DPIPELINE=$<TARGET_FILE:unit_pipeline_elf_executable>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/elf_executable_run.cmake)
endif()
