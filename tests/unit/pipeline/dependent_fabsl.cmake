# Both targets run syntax checks with the native ycxx executable.
unset(ENV{YCXX_DAEMON})
set(ENV{YCXX_NO_DAEMON} 1)

function(check_fabsl target fixture status diagnostic)
  execute_process(
    COMMAND "${YCXX}" cxx "--target=${target}" -std=c++26 -fsyntax-only ${ARGN} "${fixture}"
    RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
  if(NOT "${result}" STREQUAL "${status}")
    message(FATAL_ERROR "${target} ${fixture} exited ${result}, expected ${status}\n${errors}")
  endif()
  if("${diagnostic}" STREQUAL "")
    return()
  endif()
  string(FIND "${errors}" "${diagnostic}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "${target} ${fixture} did not report '${diagnostic}'\n${errors}")
  endif()
endfunction()

set(linux x86_64-pc-linux-gnu)
set(darwin arm64-apple-darwin)
set(core "${CMAKE_CURRENT_LIST_DIR}/../../own/floating")
set(fixtures "${CMAKE_CURRENT_LIST_DIR}/dependent-fabsl")
set(unsupported "extended floating-point types are not supported yet")

check_fabsl(${linux} "${core}/fabsl-dependent-definition.accept.cpp" 0 "")
check_fabsl(${linux} "${core}/fabsl-discarded-int.accept.cpp" 0 "")
check_fabsl(${linux} "${core}/fabsl-wrong-arity.reject.cpp" 1 "too many arguments in call")
foreach(target IN ITEMS ${linux} ${darwin})
  check_fabsl(${target} "${fixtures}/pointer-argument.cpp" 1 "cannot convert")
endforeach()

# The concrete evaluated call tests the Linux x87 guard.
check_fabsl(${linux} "${fixtures}/selected-long-double.cpp" 1 "${unsupported}")
check_fabsl(${darwin} "${fixtures}/selected-long-double.cpp" 0 "")
check_fabsl(${linux} "${fixtures}/decltype-result.cpp" 0 "")
check_fabsl(${darwin} "${fixtures}/decltype-result.cpp" 0 "")
check_fabsl(${linux} "${fixtures}/capability.cpp" 0 "" -DEXPECT_FABSL=0)
check_fabsl(${darwin} "${fixtures}/capability.cpp" 0 "" -DEXPECT_FABSL=1)
