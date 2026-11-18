ycxxc_unit_test(lex lex_test.cpp ycxxc_pp)

# Lexing throughput. Not a test: run it from an optimized build (YCXX_BUILD_TYPE=Release).
add_executable(lex_bench bench.cpp)
target_link_libraries(lex_bench PRIVATE ycxxc_pp)
