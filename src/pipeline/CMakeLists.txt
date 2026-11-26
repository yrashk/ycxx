# The pipeline (#43, #63): front end, function_ir, lower, codegen, tu and link, as engine queries.

# The runtime arm64 macOS executables link by default (#842, DECISIONS §1.3): libycxx's static
# archives, the GCC runtime members they call, and the names libycxx's link options force undefined.
# Nothing when libycxx has no static archives or the host is not arm64 Darwin.
set(YCXXC_DARWIN_RUNTIME_ARCHIVES "")
set(YCXXC_DARWIN_RUNTIME_UNDEFINED "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND NOT CMAKE_CROSSCOMPILING
   AND TARGET ycxx AND TARGET ycxx_abi)
  get_target_property(_libycxx_dir ycxx BINARY_DIR)
  set(_runtime
    "${_libycxx_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}ycxx${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${_libycxx_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}ycxx-abi${CMAKE_STATIC_LIBRARY_SUFFIX}")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC on Darwin lowers `thread_local` to libgcc's emulated TLS (`___emutls_get_address`).
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libemutls_w.a
                    OUTPUT_VARIABLE _emutls OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IS_ABSOLUTE "${_emutls}" OR NOT EXISTS "${_emutls}")
      message(FATAL_ERROR "ycxx: ${CMAKE_CXX_COMPILER} does not provide libemutls_w.a, which libycxx needs")
    endif()
    # A copy in the build: an upgrade of the GCC install removes its versioned directory.
    configure_file(${_emutls} ${CMAKE_CURRENT_BINARY_DIR}/libemutls_w.a COPYONLY)
    list(APPEND _runtime "${CMAKE_CURRENT_BINARY_DIR}/libemutls_w.a")

    # Optimized libycxx charconv calls ___ashlti3 and ___lshrti3 (#1130). Take only the
    # corresponding libgcc members so the default link keeps its other runtime providers.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-libgcc-file-name
                    OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    if(NOT IS_ABSOLUTE "${_libgcc}" OR NOT EXISTS "${_libgcc}")
      message(FATAL_ERROR "ycxx: ${CMAKE_CXX_COMPILER} does not provide libgcc.a, which libycxx needs")
    endif()
    set(_shift_dir "${CMAKE_CURRENT_BINARY_DIR}/libgcc-int128")
    set(_shift_archive "${CMAKE_CURRENT_BINARY_DIR}/libgcc-int128.a")
    file(MAKE_DIRECTORY "${_shift_dir}")
    file(REMOVE "${_shift_dir}/_ashldi3.o" "${_shift_dir}/_lshrdi3.o" "${_shift_dir}/libgcc-int128.a")
    execute_process(COMMAND ${CMAKE_AR} -x "${_libgcc}" _ashldi3.o _lshrdi3.o
                    WORKING_DIRECTORY "${_shift_dir}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND ${CMAKE_AR} -crs libgcc-int128.a _ashldi3.o _lshrdi3.o
                    WORKING_DIRECTORY "${_shift_dir}" COMMAND_ERROR_IS_FATAL ANY)
    file(RENAME "${_shift_dir}/libgcc-int128.a" "${_shift_archive}")
    list(APPEND _runtime "${_shift_archive}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_libgcc}")
  endif()
  foreach(_archive IN LISTS _runtime)
    string(REPLACE "\\" "\\\\" _archive "${_archive}")
    string(REPLACE "\"" "\\\"" _archive "${_archive}")
    string(APPEND YCXXC_DARWIN_RUNTIME_ARCHIVES "\"${_archive}\", ")
  endforeach()
  # libycxx writes its link options for programs one per line (cmake/ycxx-link.cmake).
  file(STRINGS ${_libycxx_dir}/ycxx-link-options _options REGEX "^-Wl,-u,")
  foreach(_option IN LISTS _options)
    string(REGEX REPLACE "^-Wl,-u," "" _name "${_option}")
    string(APPEND YCXXC_DARWIN_RUNTIME_UNDEFINED "\"${_name}\", ")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_libycxx_dir}/ycxx-link-options)
endif()
configure_file(runtime.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/runtime.cppm @ONLY)

add_library(ycxxc_pipeline STATIC)
target_sources(ycxxc_pipeline PUBLIC FILE_SET CXX_MODULES
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/runtime.cppm
  lines.cppm
  module_discovery.cppm
  mangle.cppm
  back.cppm
  policy_registry.cppm
  policy_objects.cppm
  front.cppm
  policies.cppm
  executable.cppm
  session.cppm
  pipeline.cppm)
target_sources(ycxxc_pipeline PRIVATE groups.cpp)
target_link_libraries(ycxxc_pipeline PUBLIC ycxxc_parse ycxxc_irgen ycxxc_aarch64 ycxxc_x86_64 ycxxc_link ycxxc_cm)
# The programs ycxx links on arm64 macOS need the runtime archives built (see above).
if(YCXXC_DARWIN_RUNTIME_ARCHIVES)
  add_dependencies(ycxxc_pipeline ycxx ycxx_abi)
endif()
