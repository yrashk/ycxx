# ycxx.pp: phase 4 and the `pp` query (#85), over ycxx.lex's phases 1–3 and 5–6.
# Use the system for the configured ycxx binary, including single-architecture Darwin builds.
set(YCXXC_HOST_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OSX_ARCHITECTURES)
  set(YCXXC_HOST_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${YCXXC_HOST_PROCESSOR}" YCXXC_HOST_PROCESSOR)
set(YCXXC_NATIVE_TARGET "nullptr")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND YCXXC_HOST_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(YCXXC_NATIVE_TARGET "&ast::x86_64_linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND YCXXC_HOST_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(YCXXC_NATIVE_TARGET "&ast::aarch64_darwin")
endif()
set(YCXX_LIBYCXX_INCLUDE ${YCXXC_LIBYCXX_SOURCE_DIR}/include)
set(YCXX_LIBYCXX_GENERATED_INCLUDE ${PROJECT_BINARY_DIR}/deps/libycxx/generated/include)
set(YCXX_LINUX_RESOURCE_INCLUDE ${PROJECT_SOURCE_DIR}/resources/linux/include)
set(YCXX_COMMON_RESOURCE_INCLUDE ${PROJECT_SOURCE_DIR}/resources/common/include)
configure_file(system.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/system.cppm @ONLY)
add_library(ycxxc_pp ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_pp PUBLIC FILE_SET CXX_MODULES
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  FILES pp.cppm spell.cppm files.cppm region.cppm unit.cppm tu.cppm predefined.cppm cond.cppm
    ${CMAKE_CURRENT_BINARY_DIR}/system.cppm)
target_link_libraries(ycxxc_pp PUBLIC ycxxc_lex ycxxc_ast)
