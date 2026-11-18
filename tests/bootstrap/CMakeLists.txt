# Bootstrap-compiler smoke test (#4): small uses of the C++26 features ycxx relies on (DECISIONS
# §3.2), compiled with the bootstrap compiler and run, so a gap shows up on day one. One program
# per feature, so a failure names the feature. A gap found here gets a `// BOOTSTRAP:` workaround
# in ycxx's sources and an entry in STATUS.
add_library(bootstrap_smoke_module STATIC)
target_sources(bootstrap_smoke_module PUBLIC FILE_SET CXX_MODULES FILES smoke_module.cppm)
target_link_libraries(bootstrap_smoke_module PUBLIC ycxxc_options)

set(_features modules reflection contracts coroutines execution language)
# Expected failures: features the bootstrap toolchain cannot compile yet, with the reason. Such a
# program is left out of the build; its test builds it and passes while the build fails, so a
# fix shows up as a failing test (remove the entry then). Each is listed in STATUS too.
foreach(feature IN LISTS _features)
  if(DEFINED _xfail_${feature})
    add_executable(bootstrap_${feature} EXCLUDE_FROM_ALL ${feature}.cpp)
    add_test(NAME bootstrap.${feature}
             COMMAND ${CMAKE_COMMAND} --build ${PROJECT_BINARY_DIR} --target bootstrap_${feature})
    set_tests_properties(bootstrap.${feature} PROPERTIES WILL_FAIL TRUE LABELS xfail)
    message(STATUS "ycxx: bootstrap smoke test '${feature}' is an expected failure: ${_xfail_${feature}}")
  else()
    add_executable(bootstrap_${feature} ${feature}.cpp)
    add_test(NAME bootstrap.${feature} COMMAND bootstrap_${feature})
  endif()
  target_link_libraries(bootstrap_${feature} PRIVATE bootstrap_smoke_module)
endforeach()
# A violated precondition reaches the replaceable violation handler.
add_executable(bootstrap_contract_violation contract_violation.cpp)
target_link_libraries(bootstrap_contract_violation PRIVATE ycxxc_options)
add_test(NAME bootstrap.contract_violation COMMAND bootstrap_contract_violation)
set_tests_properties(bootstrap.contract_violation PROPERTIES PASS_REGULAR_EXPRESSION "^violated: x > 0\n$")
# The ycxx binary starts, prints its version and exits.
add_test(NAME bootstrap.ycxx_version COMMAND ycxxc --version)
set_tests_properties(bootstrap.ycxx_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^ycxx ${PROJECT_VERSION}\n$")
add_test(NAME bootstrap.ycxx_unknown_front COMMAND ycxxc frobnicate)
set_tests_properties(bootstrap.ycxx_unknown_front PROPERTIES WILL_FAIL TRUE)

