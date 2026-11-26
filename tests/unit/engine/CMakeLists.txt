ycxxc_unit_test(engine engine_test.cpp ycxxc_engine)
ycxxc_unit_test(engine_snapshot snapshot_test.cpp ycxxc_engine)
ycxxc_unit_test(engine_contract contract_test.cpp ycxxc_engine)
set_tests_properties(unit.engine_contract PROPERTIES PASS_REGULAR_EXPRESSION "^violated: live_frames\\(\\) == 0\n$")
ycxxc_unit_test(engine_contract_concurrent contract_concurrent_test.cpp ycxxc_engine)
set_tests_properties(unit.engine_contract_concurrent PROPERTIES PASS_REGULAR_EXPRESSION "^violated: one_at_a_time\n$")

# The fetch-hit and hashing costs against the ~100 ns budget (§A3.3). Not a test: run it from an
# optimized build (YCXX_BUILD_TYPE=Release) and compare.
add_executable(engine_bench bench.cpp)
target_link_libraries(engine_bench PRIVATE ycxxc_engine)

# The scheduler (#15): thread counts, chaos, cancellation, hand-over, cycles across threads.
if(YCXXC_STDLIB STREQUAL "system")
  set(YCXXC_SCHEDULER_REPLACEMENT_NAMESPACE "ycxx::__portable_std::execution::parallel_scheduler_replacement")
else()
  set(YCXXC_SCHEDULER_REPLACEMENT_NAMESPACE "std::execution::parallel_scheduler_replacement")
endif()
configure_file(scheduler_test.cpp.in scheduler_test.cpp @ONLY)
ycxxc_unit_test(engine_scheduler "${CMAKE_CURRENT_BINARY_DIR}/scheduler_test.cpp" ycxxc_engine)

# The engine's additions for the daemon (#95): providers, durabilities, reset_unobserved, counts.
ycxxc_unit_test(engine_inputs inputs_test.cpp ycxxc_engine)

# Memory (#274): attribution to cells across workers, kinds' retained bytes, process totals.
ycxxc_unit_test(engine_memory memory_test.cpp ycxxc_engine ycxxc_new)
