cmake_minimum_required(VERSION 3.30)
project(YcxxDivisionBenchmark LANGUAGES CXX)

find_package(YCXXBench 0.1 CONFIG REQUIRED)
find_program(YCXX_COMPILER ycxx REQUIRED)
find_program(OBJCOPY objcopy REQUIRED)

set(workload "${CMAKE_CURRENT_SOURCE_DIR}/workload.cpp")
function(compile_workload mode prefix)
  set(raw "${CMAKE_CURRENT_BINARY_DIR}/${mode}.raw.o")
  set(object "${CMAKE_CURRENT_BINARY_DIR}/${mode}.o")
  set(symbol_options)
  foreach(symbol IN ITEMS shard_index page_offset lane_index disjoint volatile_load changed conditional)
    list(APPEND symbol_options --redefine-sym "${symbol}=${prefix}_${symbol}")
  endforeach()
  add_custom_command(OUTPUT "${object}" BYPRODUCTS "${raw}"
    COMMAND "${CMAKE_COMMAND}" -E env YCXX_NO_DAEMON=1 YCXX_THREADS=1
      "${YCXX_COMPILER}" cxx --target=x86_64-pc-linux-gnu
      "-fycxx-optimize=${mode}" -c "${workload}" -o "${raw}"
    COMMAND "${OBJCOPY}" ${symbol_options} "${raw}" "${object}"
    DEPENDS "${YCXX_COMPILER}" "${workload}"
    VERBATIM)
  set_source_files_properties("${object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  set("${mode}_object" "${object}" PARENT_SCOPE)
endfunction()
compile_workload(heuristic plain)
compile_workload(solver fused)

add_custom_target(division_objects DEPENDS "${heuristic_object}" "${solver_object}")

add_executable(division_compare compare.cpp "${heuristic_object}" "${solver_object}")
add_dependencies(division_compare division_objects)
target_link_libraries(division_compare PRIVATE YCXXBench::bench ycxx::modules)

foreach(mode IN ITEMS heuristic solver)
  if(mode STREQUAL "heuristic")
    set(entry plain_shard_index)
  else()
    set(entry fused_shard_index)
  endif()
  add_executable("division_${mode}_rss" memory.cpp "${${mode}_object}")
  add_dependencies("division_${mode}_rss" division_objects)
  target_compile_definitions("division_${mode}_rss" PRIVATE ENTRY=${entry} LABEL="${mode}")
  target_link_libraries("division_${mode}_rss" PRIVATE YCXXBench::bench ycxx::modules)
endforeach()
