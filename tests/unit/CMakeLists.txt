# Unit tests of ycxx's own components, one directory per area. Each test is an executable run by
# ctest, so tools/build (and with it tools/test's build stage and CI) runs them.
add_library(ycxxc_test_check STATIC)
target_sources(ycxxc_test_check PUBLIC FILE_SET CXX_MODULES FILES check.cppm)
target_link_libraries(ycxxc_test_check PUBLIC ycxxc_options)

# ycxxc_unit_test(<area> <name>): <area>/<name>.cpp, linked with `libs`, run as unit.<area>.<name>.
function(ycxxc_unit_test area name)
  add_executable(unit_${area}_${name} ${area}/${name}.cpp)
  target_link_libraries(unit_${area}_${name} PRIVATE ycxxc_test_check ${ARGN})
  add_test(NAME unit.${area}.${name} COMMAND unit_${area}_${name})
endfunction()

ycxxc_unit_test(ast handles ycxxc_ast)
