ycxxc_unit_test(cm cm_test.cpp ycxxc_cm)

set(YCXXC_FLATZINC_SOLVER "" CACHE FILEPATH "Optional fzn-chuffed for the FlatZinc round-trip test")
if(YCXXC_FLATZINC_SOLVER)
  add_test(NAME unit.cm_flatzinc
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/flat_zinc_test.py
      $<TARGET_FILE:unit_cm> "${YCXXC_FLATZINC_SOLVER}")
endif()

add_executable(ycxx-cm-export-bench EXCLUDE_FROM_ALL export_bench.cpp)
target_link_libraries(ycxx-cm-export-bench PRIVATE YCXXBench::bench ycxxc_cm)
