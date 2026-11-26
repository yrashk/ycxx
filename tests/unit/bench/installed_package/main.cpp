import std;
import ycxx.bench;
import ycxx.bench.experiments;

int main() {
  using namespace std::chrono_literals;
  using namespace ycxx::bench;
  using namespace ycxx::bench::experiments;

  Configuration config;
  config.warmup_time = 0ns;
  config.measurement_time = 1ns;
  config.minimum_sample_time = 1ns;
  config.samples = 3;
  config.maximum_iterations = 1;

  std::uint64_t calls = 0;
  const auto measured = measure([&] { return ++calls; }, config);
  if (calls < config.samples || measured.samples.size() != config.samples) return 1;

  Recorder recorder("installed benchmark", 2);
  recorder.record(40ns, Outcome::success);
  recorder.record(20ns, Outcome::success);
  const auto samples = recorder.snapshot().samples();
  if (samples.size() != 2 || samples[0].elapsed != 40ns) return 2;

  // Call a non-inline function of the experiments module so its archive
  // symbols, as well as its installed interface, must reach the consumer.
  const auto comparison = compare_independent(std::span(samples).first(1), std::span(samples).last(1));
  if (!(comparison.speedup > 1.99 && comparison.speedup < 2.01)) return 3;
}
