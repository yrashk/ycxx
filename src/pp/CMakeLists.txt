# ycxx.pp: phase 4 and the `pp` query (#85), over ycxx.lex's phases 1–3 and 5–6.
set(YCXX_LIBYCXX_INCLUDE ${PROJECT_SOURCE_DIR}/deps/libycxx/include)
set(YCXX_LIBYCXX_GENERATED_INCLUDE ${PROJECT_BINARY_DIR}/deps/libycxx/generated/include)
configure_file(system.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/system.cppm @ONLY)
add_library(ycxxc_pp STATIC)
target_sources(ycxxc_pp PUBLIC FILE_SET CXX_MODULES
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  FILES pp.cppm spell.cppm files.cppm region.cppm unit.cppm tu.cppm predefined.cppm
    ${CMAKE_CURRENT_BINARY_DIR}/system.cppm)
target_link_libraries(ycxxc_pp PUBLIC ycxxc_lex ycxxc_ast)
