# Optimizer benchmarks

`ycxx.bench` is a C++26 module for small, repeatable experiments. Its measurement,
statistical, and terminal designs adapt
[Flyology Bench](https://github.com/flyology-ada/flyology/tree/9396874debed77db057a8dfce77819f9a69389fa/flyology_bench).
The package can also be installed or built as a dependency in another project.
[Package guide](README.md) · [Upstream guide](https://flyology.org/guide/benchmarking/) · [MIT notice](LICENSE-FLYOLOGY)

From a ycxx checkout, build and run the demo and its unit tests.

```sh
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target ycxx-bench-demo unit_bench
ctest --test-dir build-bench -R '^unit\.bench$' --output-on-failure
build-bench/bin/ycxx-bench-demo
build-bench/bin/ycxx-bench-demo --shootout
build-bench/bin/ycxx-bench-demo --details
build-bench/bin/ycxx-bench-demo --json-live --shootout
build-bench/bin/ycxx-bench-demo --list --tag lcg
build-bench/bin/ycxx-bench-demo --group bitwise --band smoke
build-bench/bin/ycxx-bench-demo --json > paired.ndjson
build-bench/bin/ycxx-bench-demo --csv > samples.csv
```

The terminal runner displays warmup, calibration, sampling and analysis progress
on an interactive terminal. Comparisons show the verdict and effect plot first.
Use `--details` for case properties, latency, and resource cards. NDJSON includes
the policy, clock, host, analysis and integer-nanosecond samples. CSV records
every sample of a single measurement.

`--json-live` flushes one versioned NDJSON record at a time. It emits `start`,
`progress`, and `sample` records during collection and ends with the full `report`.
`sample` records carry the case name, one-based round, and raw batch data.
C++ runners can connect the live writers to `Configuration::progress` and
`Configuration::sample_observer`. Callbacks run outside timed work.

## Measure the operation that changed

```cpp
import std;
import ycxx.bench;

int main() {
  using namespace ycxx::bench;
  std::uint32_t a = 1;
  std::uint32_t b = 1;
  auto result = compare(
    [&] { return a = a * 1'664'525u + 1'013'904'223u; },
    [&] { return b = std::rotl(b * 1'664'525u + 1'013'904'223u, 7); });
  write_console(std::cout, "mix", "mix and rotate", result);
}
```

Use `measure` or `compare` for statically bound operations. The runner makes each
non-void result observable before the clock read. For pure code, supply varying
inputs that the optimizer cannot replace with a constant. Use `measure_batched`
when the workload owns the loop. Its setup and teardown hooks run outside the
timer. A batch return value should depend on every operation in the batch.
For comparisons and shootouts, use `compare_batched` or `compare_many_batched`.
The latter rotates two to sixteen cases through each sampling position. Use
shared iterations when state or work identity requires identical batch counts.

## Describe and select cases

A callable can opt in by defining a const `benchmark_properties()` or
`benchmark_metadata()`. Those concepts work with result-returning callables and
batched callables. A plain lambda can use `with_properties` or `with_metadata`.
For example, this case records typed details and opts into shorter collection.

```cpp
auto tagged = ycxx::bench::with_properties(
  [&] { return serialize(model); },
  {"flat-zinc", {"export", "linear"}, ycxx::bench::Band::smoke},
  {{"constraints", std::uint64_t{constraint_count}}});
auto result = ycxx::bench::measure(tagged);
```

Groups and tags drive case selection. The runner's `--list` command can show
cases selected by `--group`, repeated `--tag` filters, and `--band`. Each tag
filter must match. Metadata is read once before the first warmup or timer
probe. It follows its case in the terminal and machine reports.

A supplied `Configuration` controls the measurement. Otherwise the group of
cases picks one common band, with the strongest requested band winning among
`smoke`, `standard`, and `deep`. Cases that inherit follow the selected band.
An all-inherit run uses standard. Reports include each requested band and the
sampling settings used for the run.

| Band | Warmup target | Collection target per case | Samples |
| --- | ---: | ---: | ---: |
| `smoke` | 15 ms | 60 ms | 15 |
| `standard` | 100 ms | 500 ms | 50 |
| `deep` | 500 ms | 3 s | 120 |

## Read the evidence

- The runner warms code and data, then calibrates until three batches reach the
  target or the iteration cap is hit. Each case then keeps its calibrated count.
  The default uses 100 ms warmup and 50 samples with a 500 ms target per case.
  A collection limit is checked between complete rounds after ten samples.
- Timing uses `CLOCK_MONOTONIC_RAW` on Linux and `mach_absolute_time` on Darwin.
  The clock card shows nominal resolution, minimum positive adjacent-read delta,
  median adjacent-read cost, and clock floor per operation. Subtraction is opt-in
  and applies once per batch. Fractional nanoseconds are amortized batch
  estimates. The p95 and p99 describe the retained batch means.
- Outliers remain in the measurement. MAD, CV, lag-one correlation, the
  sample sparkline, and optional telemetry help interpret noise. Process and
  thread CPU, resident RSS at both boundaries, peak RSS, page faults, context
  switches, block I/O operations, and observed host CPU load accompany the raw
  batch. Peak RSS is the process high-water mark. Process CPU can exceed 100%
  for parallel work. Current RSS is a boundary snapshot and can miss an
  allocation released within a batch. Unavailable native probes stay empty.
- Paired runs alternate execution order and report the geometric mean of
  reference-to-contender time ratios. Circular-block resampling of paired log
  ratios gives the interval. A speedup above 1 favors the contender. A verdict
  requires the interval to clear the configured practical threshold. Fewer
  than ten pairs, or any non-positive elapsed value, gives no interval.
- Capture raw observations and build revision together. Compare one change on
  the same corpus. Measure compile-time cost and generated-code runtime
  independently, and retain code size and correctness checks for the same
  workload. Compare predicted optimizer cost with measured runtime before
  promoting a heuristic. The compiler-wide comparison is tracked in
  [yrashk/ycxx#9](https://github.com/yrashk/ycxx/issues/9).

## Sweep, record, and compare stored observations

Import `ycxx.bench.experiments` for typed size and count points, work per
operation, derived throughput, and exploratory fits to common growth curves.
A sweep retains skipped and failed points along with measured points. The same
module provides a bounded external recorder with success, failure, timeout,
cancellation, and abandoned-span outcomes. Use independent comparison for
unpaired recordings from separate executions. Use paired comparison for cases
that shared sampling rounds.
