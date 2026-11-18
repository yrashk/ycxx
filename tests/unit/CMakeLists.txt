# Unit tests: one CTest program per area, run by tools/build (and so by tools/test's build stage).
add_library(ycxxc_unit STATIC)
target_sources(ycxxc_unit PUBLIC FILE_SET CXX_MODULES FILES support/check.cppm)
target_link_libraries(ycxxc_unit PUBLIC ycxxc_options)

# ycxxc_unit_test(<name> <source> <libraries>...): builds unit.<name> and registers it.
function(ycxxc_unit_test name source)
  add_executable(unit_${name} ${source})
  target_link_libraries(unit_${name} PRIVATE ycxxc_unit ${ARGN})
  add_test(NAME unit.${name} COMMAND unit_${name})
endfunction()

add_subdirectory(hash)
add_subdirectory(ast)
add_subdirectory(engine)
add_subdirectory(lex)
add_subdirectory(items)
add_subdirectory(link)
