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

add_test(NAME unit.driver_queries
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/driver_queries.py $<TARGET_FILE:ycxxc>)

set(_unit_subdirs
  hash bench cm ast engine lex ir items irgen interp link obj sema codegen pp pipeline trace
  build_action daemon lsp)
foreach(_dir IN LISTS _unit_subdirs)
  add_subdirectory(${_dir})
  get_directory_property(_sub_tests DIRECTORY ${_dir} TESTS)
  foreach(_t IN LISTS _sub_tests)
    set_property(TEST "${_t}" DIRECTORY ${_dir} APPEND PROPERTY LABELS "unit;area:${_dir}")
  endforeach()
endforeach()
ycxxc_unit_test(build_manifest build_manifest/manifest_test.cpp ycxxc_build_manifest)
ycxxc_unit_test(build_publication build_publication/publication_test.cpp ycxxc_project_build)
set_property(TEST unit.driver_queries APPEND PROPERTY LABELS "unit;area:driver")
set_property(TEST unit.build_manifest unit.build_publication APPEND PROPERTY LABELS "unit;area:project_build")
