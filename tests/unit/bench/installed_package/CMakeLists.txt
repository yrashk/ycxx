cmake_minimum_required(VERSION 3.30)
project(YCXXBenchConsumer LANGUAGES CXX)

find_package(YCXXBench 0.1 CONFIG REQUIRED)
add_executable(bench-consumer main.cpp)
target_link_libraries(bench-consumer PRIVATE YCXXBench::bench)

enable_testing()
add_test(NAME installed-bench-consumer COMMAND bench-consumer)
