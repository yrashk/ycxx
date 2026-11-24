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
set(ownership_include ${PROJECT_SOURCE_DIR}/packages/ycxx-policies/include)
add_test(NAME unit.policies_ownership
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_ownership.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/examples/affine ${ownership_include} --target=arm64-apple-darwin)
add_test(NAME unit.policies_contracts
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_contracts.py
          $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${ownership_include} ${PROJECT_SOURCE_DIR}/examples/affine)
add_test(NAME unit.policies_variants
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_variants.py
          $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${ownership_include})
add_test(NAME unit.policies_effects
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_effects.py
          $<TARGET_FILE:ycxxc> ${ownership_include})
add_test(NAME unit.policies_grades
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_grades.py
          $<TARGET_FILE:ycxxc> ${ownership_include})
add_test(NAME unit.policies_package
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_package.py
          ${CMAKE_COMMAND} ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/packages/ycxx-policies)
add_test(NAME unit.dependent_fabsl
  COMMAND ${CMAKE_COMMAND} -DYCXX=$<TARGET_FILE:ycxxc>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/dependent_fabsl.cmake)
# Both targets' objects, on every host: the string builtins' C symbols appear once (#248).
add_test(NAME unit.string_builtin_symbols
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/string_builtin_symbols.py $<TARGET_FILE:ycxxc>)
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
  # The Darwin counterpart of unit.linux_archive (#842), checked against the system linker.
  add_test(NAME unit.darwin_archive
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_archive.py
            $<TARGET_FILE:ycxxc> ${CMAKE_AR} ${CMAKE_C_COMPILER})
  # libycxx's runtime in a default link, and -nostdlib++ without it (#842).
  add_test(NAME unit.darwin_runtime
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_runtime.py $<TARGET_FILE:ycxxc> ${YCXXC_NM})
  # The Mach-O counterpart of the linux_* virtual tests, over their fixtures (#650). Its peers
  # are Clang's, whose libc++ and RTTI conventions are the platform's C++ ABI.
  find_program(YCXXC_CLANGXX NAMES clang++ REQUIRED)
  add_test(NAME unit.darwin_virtual
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_virtual.py
            $<TARGET_FILE:ycxxc> ${YCXXC_CLANGXX} ${YCXXC_NM})
  # The Mach-O counterpart of unit.linux_namespace_visibility and linux_inline's weak providers,
  # checked against Clang's symbol tables.
  add_test(NAME unit.darwin_weak_visibility
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/darwin_weak_visibility.py
            $<TARGET_FILE:ycxxc> ${YCXXC_CLANGXX} ${YCXXC_NM})
endif()
# PARITY: #647, #649 for the Darwin counterparts of unit.linux_variadic_definitions and
# unit.linux_array_delete; the rest by design: they run x86-64 ELF executables, which only Linux
# can. unit.darwin_virtual runs the virtual tests' fixtures on Mach-O (#650).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME unit.policies_ownership_linux
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/policies_ownership.py
            $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/examples/affine ${ownership_include} --target=x86_64-linux-gnu)
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
  # PARITY: by design: checks ELF symbols and links with a native x86-64 peer.
  add_test(NAME unit.linux_single_linkage_const
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_single_linkage_const.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_nontrivial_result
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_nontrivial_result.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
  add_test(NAME unit.linux_scalar_sse
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_scalar_sse.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/deps/libycxx)
  # PARITY: by design: exercises the System V AMD64 argument convention with native Linux peers.
  add_test(NAME unit.linux_sysv_reference_fields
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_sysv_reference_fields.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR}/deps/libycxx)
  # PARITY: by design: links x86-64 ELF objects against the host Itanium C++ ABI runtime.
  add_test(NAME unit.linux_borrowed_typed_downcast
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_borrowed_typed_downcast.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  add_test(NAME unit.linux_virtual_consumer
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_virtual_consumer.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: #294 for the Darwin borrowed cast-to-void counterpart.
  add_test(NAME unit.linux_dynamic_cast_void
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_dynamic_cast_void.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links maker objects with native x86-64 ELF peers.
  add_test(NAME unit.linux_peer_root_default_construction
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_peer_root_default_construction.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: links a derived maker object with native x86-64 ELF peers.
  add_test(NAME unit.linux_peer_derived_default_construction
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_peer_derived_default_construction.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
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
  # PARITY: by design: checks ELF visibility and runs the Linux x86-64 link. Its Mach-O counterpart
  # is unit.darwin_weak_visibility.
  add_test(NAME unit.linux_namespace_visibility
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_namespace_visibility.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF} ${PROJECT_SOURCE_DIR}/deps/libycxx)
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
  # PARITY: by design: .hidden is the ELF spelling. Mach-O uses .private_extern.
  add_test(NAME unit.linux_constant_asm
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_constant_asm.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_inline
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_inline.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF} ${CMAKE_AR}
            ${PROJECT_SOURCE_DIR}/deps/libycxx)
  # PARITY: by design: checks ELF symbols and runs the x86-64 Linux executable.
  add_test(NAME unit.linux_immediate_functions
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_immediate_functions.py
            $<TARGET_FILE:ycxxc> ${CMAKE_READELF})
  add_test(NAME unit.linux_elf_comdat
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_elf_comdat.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_AR})
  # PARITY: by design: links and runs x86-64 ELF virtual calls with native peers.
  add_test(NAME unit.linux_pure_virtual_dispatch
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_pure_virtual_dispatch.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF})
  # PARITY: by design: checks x86-64 INT3 and Linux SIGTRAP delivery.
  add_test(NAME unit.linux_basic_asm
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_basic_asm.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_OBJDUMP})
  add_test(NAME unit.linux_std_mangling
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_std_mangling.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER} ${CMAKE_READELF}
            ${PROJECT_SOURCE_DIR}/deps/libycxx)
  add_test(NAME unit.linux_dynamic
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_dynamic.py
            $<TARGET_FILE:ycxxc> ${CMAKE_C_COMPILER} ${CMAKE_READELF})
  # PARITY: #294 for the Darwin borrowed typed downcast counterpart.
  add_test(NAME unit.linux_keyless_base_rtti
    COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/linux_keyless_base_rtti.py
            $<TARGET_FILE:ycxxc> ${CMAKE_CXX_COMPILER})
  add_test(NAME unit.linux_stdarg
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linux_stdarg.py $<TARGET_FILE:ycxxc>)
  add_test(NAME unit.pipeline_elf_run
    COMMAND ${CMAKE_COMMAND} -DPIPELINE=$<TARGET_FILE:unit_pipeline_elf_executable>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/elf_executable_run.cmake)
endif()
