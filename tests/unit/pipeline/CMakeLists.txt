# The pipeline (#43). On arm64 macOS, unit.pipeline also links and runs what it compiles.
add_executable(unit_pipeline pipeline_test.cpp)
target_link_libraries(unit_pipeline PRIVATE ycxxc_unit ycxxc_pipeline)
# PARITY: by design: runs the arm64 Mach-O executables it links, which only macOS can
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
# PARITY: by design: all hosts check the emitted and linked arm64 Mach-O records.
# Only an arm64 macOS host can run the Mach-O exception propagation test.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND NOT CMAKE_CROSSCOMPILING)
  set(darwin_unwind_host ${CMAKE_CXX_COMPILER})
endif()
add_test(NAME unit.darwin_cleanup_free_unwind
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_cleanup_free_unwind.py
          $<TARGET_FILE:ycxxc> ${darwin_unwind_host})
add_test(NAME unit.darwin_local_unwind
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_local_unwind.py
          $<TARGET_FILE:ycxxc> ${darwin_unwind_host})
# PARITY: by design: runs what it links with the host's linker; the Linux block registers its own
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
# PARITY: #647, #649, #650 for the Darwin counterparts of unit.linux_variadic_definitions,
# unit.linux_array_delete and unit.linux_virtual_consumer; the rest by design: they run x86-64
# ELF executables, which only Linux can
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
  add_test(NAME unit.linux_stack_probe
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_stack_probe.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER})
  add_test(NAME unit.linux_scalar_sse
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_scalar_sse.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_virtual_consumer
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_virtual_consumer.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links maker objects with native x86-64 ELF peers.
  add_test(NAME unit.linux_peer_root_default_construction
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_peer_root_default_construction.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links a derived maker object with native x86-64 ELF peers.
  add_test(NAME unit.linux_peer_derived_default_construction
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_peer_derived_default_construction.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: #650 for the Darwin owned derived virtual table and RTTI counterpart.
  add_test(NAME unit.linux_owned_derived_vtable
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_owned_derived_vtable.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_LINKER})
  add_test(NAME unit.linux_function_addresses
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_function_addresses.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links and runs x86-64 ELF objects with native peers.
  add_test(NAME unit.linux_virtual_member_final
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_virtual_member_final.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
  add_test(NAME unit.tu_local_parameter_symbols
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tu_local_parameter_symbols.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} linux ${CMAKE_READELF})
  add_test(NAME unit.linux_executable
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_executable.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_cleanup_free_unwind
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_cleanup_free_unwind.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF} ${CMAKE_OBJDUMP})
  add_test(NAME unit.linux_native_int_throw
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_native_int_throw.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_local_unwind
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_local_unwind.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_union_constructor_unwind
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_union_constructor_unwind.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links the ELF base and complete object entries with native peers.
  add_test(NAME unit.linux_base_object_variants
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_base_object_variants.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links virtual destructor entries with native x86-64 ELF peers.
  add_test(NAME unit.linux_root_virtual_destructor
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_root_virtual_destructor.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_executable(unit_linux_hosted_catch_all_object linux_hosted_catch_all_object.cpp)
  target_link_libraries(unit_linux_hosted_catch_all_object PRIVATE ycxxc_x86_64 ycxxc_obj)
  add_test(NAME unit.linux_hosted_catch_all_object
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_hosted_catch_all.py
            $<TARGET_FILE:unit_linux_hosted_catch_all_object> $<TARGET_FILE:ycxxc>
            ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: #648 for the Darwin typed-handler counterpart.
  add_test(NAME unit.linux_hosted_int_catch
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_hosted_int_catch.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
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
  # PARITY: by design: links and runs x86-64 ELF virtual calls with native peers.
  add_test(NAME unit.linux_pure_virtual_dispatch
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_pure_virtual_dispatch.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
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
