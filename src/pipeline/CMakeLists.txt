# The pipeline (#43, #63): front end, function_ir, lower, codegen, tu and link, as engine queries.

# The runtime arm64 macOS executables link by default (#842, DECISIONS §1.3): libycxx's static
# archives, the GCC-built emulated TLS they call, and the names libycxx's link options force
# undefined. Nothing when libycxx has no static archives or the host is not arm64 Darwin.
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
  mangle.cppm
  back.cppm
  front.cppm
  executable.cppm
  session.cppm
  pipeline.cppm)
target_link_libraries(ycxxc_pipeline PUBLIC ycxxc_parse ycxxc_irgen ycxxc_aarch64 ycxxc_x86_64 ycxxc_link)
# The programs ycxx links on arm64 macOS need the runtime archives built (see above).
if(YCXXC_DARWIN_RUNTIME_ARCHIVES)
  add_dependencies(ycxxc_pipeline ycxx ycxx_abi)
endif()
