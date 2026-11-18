ycxxc_unit_test(engine engine_test.cpp ycxxc_engine)
ycxxc_unit_test(engine_contract contract_test.cpp ycxxc_engine)
set_tests_properties(unit.engine_contract PROPERTIES PASS_REGULAR_EXPRESSION "^violated: live_frames\\(\\) == 0\n$")

# The fetch-hit and hashing costs against the ~100 ns budget (§A3.3). Not a test: run it from an
# optimized build (YCXX_BUILD_TYPE=Release) and compare.
add_executable(engine_bench bench.cpp)
target_link_libraries(engine_bench PRIVATE ycxxc_engine)

# The scheduler (#15): thread counts, chaos, cancellation, hand-over, cycles across threads.
ycxxc_unit_test(engine_scheduler scheduler_test.cpp ycxxc_engine)

# The engine's additions for the daemon (#95): providers, durabilities, reset_unobserved, counts.
ycxxc_unit_test(engine_inputs inputs_test.cpp ycxxc_engine)
