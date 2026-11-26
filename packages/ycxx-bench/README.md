# YCXX Bench

An adaptive C++26 benchmarking module for Linux and macOS. It includes warmup,
batch calibration, balanced comparisons, raw samples, and a terminal reporter.
It exposes `YCXXBench::bench` to CMake and `ycxx.bench` to C++.

## Install

Use CMake 3.30 or later, Ninja, GCC 16.2 or later, and libycxx built and installed
with `YCXX_MODULES=ON`. Use the same compiler and standard-library configuration
for the package and its C++ consumers. The package directory can be built from
its own checkout or copied into another project. [libycxx](https://github.com/yrashk/libycxx)

```sh
cmake -S packages/ycxx-bench -B build-bench-package -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-16 \
  -DCMAKE_PREFIX_PATH=/path/to/libycxx-prefix \
  -DCMAKE_INSTALL_PREFIX="$PWD/stage/ycxx-bench"
cmake --build build-bench-package
cmake --install build-bench-package

stage/ycxx-bench/bin/ycxx-bench-demo --shootout
stage/ycxx-bench/bin/ycxx-bench-demo --json-live
stage/ycxx-bench/bin/ycxx-bench-demo --list --tag lcg
```

The package installs its static archive and module interface source. CMake builds
the module interface for the consumer with its own compiler and configuration.

## Use from another project

Put the benchmark and libycxx prefixes on `CMAKE_PREFIX_PATH` and link the
exported target.

```cmake
cmake_minimum_required(VERSION 3.30)
project(MyBenchmark LANGUAGES CXX)
find_package(YCXXBench 0.1 CONFIG REQUIRED)
add_executable(my_benchmark main.cpp)
target_link_libraries(my_benchmark PRIVATE YCXXBench::bench)
```

For a source dependency, use `add_subdirectory(vendor/ycxx-bench)` in place
of `find_package(YCXXBench)`. This uses an existing `ycxx::modules` target or
finds libycxx on `CMAKE_PREFIX_PATH`. Link the same benchmark target.

```cpp
import std;
import ycxx.bench;

int main() {
  using namespace ycxx::bench;
  std::uint32_t value = 1;
  auto mix = with_properties(
    [&] { return value = value * 1'664'525u + 1'013'904'223u; },
    {"arithmetic", {"integer", "throughput"}, Band::standard},
    {{"word_bits", std::uint64_t{32}}});
  write_console(std::cout, "integer mix", measure(mix));
}
```

The module also provides paired comparisons, multi-case runs, NDJSON and CSV.
The runner guide explains sampling policies, callable properties, and how to read
the display. [Benchmark runner guide](GUIDE.md)

The measurement and reporting design adapts Flyology Bench. Its MIT notice is
distributed alongside this package. [Flyology Bench](https://flyology.org/guide/benchmarking/)
