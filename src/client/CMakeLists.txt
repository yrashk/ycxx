# The c++-compatible request handler (ycxx.compile), shared by the daemon and the in-process path.
# Use the system for the configured ycxx binary, including single-architecture Darwin builds.
set(YCXXC_HOST_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OSX_ARCHITECTURES)
  set(YCXXC_HOST_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${YCXXC_HOST_PROCESSOR}" YCXXC_HOST_PROCESSOR)
set(YCXXC_DEFAULT_TARGET "nullptr")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND YCXXC_HOST_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(YCXXC_DEFAULT_TARGET "&ast::x86_64_linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND YCXXC_HOST_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(YCXXC_DEFAULT_TARGET "&ast::aarch64_darwin")
endif()
configure_file(host_target.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/host_target.cppm @ONLY)

add_library(ycxxc_compile STATIC)
target_sources(ycxxc_compile PUBLIC FILE_SET CXX_MODULES
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  FILES compile.cppm ${CMAKE_CURRENT_BINARY_DIR}/host_target.cppm)
target_link_libraries(ycxxc_compile PUBLIC ycxxc_pipeline)
