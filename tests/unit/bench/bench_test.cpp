import std;
import ycxx.bench;
import ycxx.unit;

using namespace ycxx::bench;
using ycxx::unit::check;

namespace {

void statistics() {
  std::vector<Sample> samples(9, Sample{2, Nanoseconds(20), {}});
  samples.push_back({2, Nanoseconds(200), {}});
  const auto summary = summarize(samples);
  check(summary.minimum == 10 && summary.median == 10 && summary.maximum == 100, "sample means and range");
  check(std::abs(summary.mean - 19) < 1e-12 && summary.mad == 0, "mean and MAD");
  check(summary.severe_outliers == 1 && summary.mild_outliers == 0, "Tukey counts retain the outlier");
  check(std::abs(summary.p95 - 59.5) < 1e-10 && std::abs(summary.p99 - 91.9) < 1e-10, "interpolated batch tails");
  check(summary.mean_interval.has_value(), "mean interval available for ten samples");
  check(!summarize(std::span(samples).first(9)).mean_interval, "mean interval withheld for nine samples");

  std::vector<Sample> reference;
  std::vector<Sample> contender;
  for (int i = 1; i <= 16; ++i) {
    reference.push_back({10, Nanoseconds(i * 400), {}});
    contender.push_back({40, Nanoseconds(i * 800), {}});
  }
  const auto paired = summarize_paired(reference, contender);
  check(std::abs(paired.speedup - 2) < 1e-12, "paired ratios use each side's operation count");
  check(paired.speedup_interval && std::abs(paired.speedup_interval->low - 2) < 1e-12
    && std::abs(paired.speedup_interval->high - 2) < 1e-12, "deterministic block interval for constant ratios");
  check(paired.verdict == Verdict::contender_faster, "speedup direction");
  check(summarize_paired(contender, reference).verdict == Verdict::reference_faster, "reverse direction");
  check(summarize_paired(reference, reference).verdict == Verdict::equivalent, "practical equivalence");
  contender.front().elapsed = {};
  check(summarize_paired(reference, contender).verdict == Verdict::unavailable, "zero time has no meaningful ratio");
  check(samples[0].ns_per_operation(Nanoseconds(21)) == 0, "optional timer cost clamps adjusted time");
  for (std::size_t i = 0; i < reference.size(); ++i) {
    reference[i] = {1, Nanoseconds(10'000), {}};
    contender[i] = {1, Nanoseconds(i < 8 ? 9'200 : 11'000), {}};
  }
  const auto drift = summarize_paired(reference, contender);
  check(drift.verdict == Verdict::inconclusive && drift.speedup_interval
    && drift.speedup_interval->low < 0.99 && drift.speedup_interval->high > 1.01,
    "serial drift crosses the practical threshold and stays inconclusive");

}

void sampling() {
  Configuration config;
  config.warmup_time = Nanoseconds(1);
  config.measurement_time = Nanoseconds(1);
  config.minimum_sample_time = Nanoseconds(1);
  config.samples = 12;
  config.maximum_iterations = 1;
  config.maximum_sampling_time = Nanoseconds(1);
  config.batching = Batching::shared_iterations;
  config.seed = 0;
  bool collecting = false;
  bool warmed = false;
  config.progress = [&](Progress progress) {
    collecting = progress.phase == Phase::sampling;
    warmed |= progress.phase == Phase::warming;
  };
  std::vector<std::size_t> order;
  int fixture = -1;
  std::vector<std::pair<std::size_t, std::size_t>> observed;
  std::ostringstream live;
  config.sample_observer = [&](std::size_t which, std::size_t round, const Sample& sample) {
    check(fixture == -1 && collecting, "live observer runs after teardown in the sampling phase");
    observed.emplace_back(which, round);
    write_live_sample(live, "case", round, sample);
  };
  auto result = compare_many_batched(3, [&](std::size_t which, Iterations count) {
    check(fixture == int(which) && count == 1, "fixture prepared outside timed invocation");
    if (collecting) order.push_back(which);
    return which;
  }, config, [&](std::size_t which) {
    check(fixture == -1, "previous fixture was released");
    fixture = int(which);
  }, [&](std::size_t which) {
    check(fixture == int(which), "fixture still present on teardown");
    fixture = -1;
  });
  check(warmed && fixture == -1, "warmup ran and final fixture closed");
  check(order.size() == 30 && result.cases.front().sampling_limited, "collection limit stops after ten full rounds");
  check(observed.size() == order.size(), "every retained sample is published once");
  for (std::size_t i = 0; i < order.size(); ++i) {
    check(order[i] == (i / 3 + i % 3) % 3, "each case rotates through execution positions");
    if (i < observed.size())
      check(observed[i] == std::pair(order[i], i / 3), "live events retain case and round order");
  }
  check(live.str().contains("\"event\":\"sample\"") && live.str().contains("\"round\":10"),
    "live reporter emits sample events with one-based rounds");
  for (const auto& measurement : result.cases)
    check(measurement.samples.size() == 10 && !measurement.clock.source.empty(), "each case retained every round");

  std::ostringstream report;
  write_ndjson(report, "a\"b\nc", result.cases.front());
  const auto ndjson = report.str();
  check(ndjson.contains("\"case\":\"a\\\"b\\u000ac\"") && ndjson.contains("\"samples\":[{")
    && ndjson.contains("\"elapsed_ns\":"), "NDJSON escapes identity and retains raw samples");
}


void memory_telemetry() {
  using namespace std::chrono_literals;
  constexpr std::size_t bytes = 16u * 1024 * 1024;
  Configuration config;
  config.warmup_time = 0ns;
  config.measurement_time = 1ns;
  config.minimum_sample_time = 1ns;
  config.samples = 1;
  config.maximum_iterations = 1;
  config.process_telemetry = true;

  std::unique_ptr<unsigned char[]> allocation;
  auto result = measure([&] {
    allocation.reset(new unsigned char[bytes]);
    volatile unsigned char* visible = allocation.get();
    for (std::size_t i = 0; i < bytes; i += 4096) visible[i] = 1;
    return allocation.get();
  }, config);
  check(result.samples.size() == 1 && result.samples.front().process.has_value(),
    "one process-telemetry sample retained");
  if (result.samples.empty() || !result.samples.front().process) return;

  const auto& process = *result.samples.front().process;
  check(process.rss_source != RssSource::unspecified && process.rss_before_bytes && process.rss_after_bytes
    && process.peak_rss_bytes, "native snapshot supplies current RSS, high-water and provenance");
  if (process.rss_before_bytes && process.rss_after_bytes && process.peak_rss_bytes) {
    check(*process.rss_after_bytes >= *process.rss_before_bytes + bytes / 2,
      "boundary RSS observes touched pages retained by the workload");
    check(*process.peak_rss_bytes >= *process.rss_after_bytes,
      "native lifetime high-water covers the current RSS in the same snapshot");
  }
  std::ostringstream machine;
  write_ndjson(machine, "touched pages", result);
  check(machine.str().contains("\"peak_rss_scope\":\"process_lifetime\"")
    && (machine.str().contains("\"rss_source\":\"linux_proc_status\"")
      || machine.str().contains("\"rss_source\":\"mach_task_basic_info\"")),
    "NDJSON discloses which high-water counter was measured");

  // A resource counter can be available when the resident-memory probe fails.
  auto& missing = *result.samples.front().process;
  missing.rss_source = RssSource::unspecified;
  missing.peak_rss_bytes.reset();
  missing.rss_before_bytes.reset();
  missing.rss_after_bytes.reset();
  missing.rusage_max_rss_bytes = bytes;
  std::ostringstream fallback;
  write_ndjson(fallback, "missing RSS snapshot", result);
  check(fallback.str().contains("\"peak_rss_bytes\":null")
    && fallback.str().contains("\"peak_rss_scope\":null")
    && fallback.str().contains("\"rusage_max_rss_bytes\":16777216"),
    "an available resource counter does not fill an unavailable native high-water snapshot");
}

struct DescribedCase {
  int calls = 0;
  mutable int metadata_calls = 0;
  mutable int properties_calls = 0;
  Band band = Band::smoke;

  Metadata benchmark_metadata() const {
    ++metadata_calls;
    check(calls == 0, "metadata captured before workload execution");
    return {{"input bytes", std::uint64_t{128}}};
  }

  Properties benchmark_properties() const {
    ++properties_calls;
    check(calls == 0, "properties captured before workload execution");
    return {"format", {"flat-zinc", "small"}, band};
  }

  int operator()() { return ++calls; }
};

static_assert(HasBenchmarkMetadata<DescribedCase> && HasBenchmarkProperties<DescribedCase>);

void descriptions() {
  using namespace std::chrono_literals;
  Configuration config;
  config.warmup_time = 0ns;
  config.measurement_time = 1ns;
  config.minimum_sample_time = 1ns;
  config.samples = 2;
  config.maximum_iterations = 1;

  DescribedCase a;
  DescribedCase b{.band = Band::deep};
  const auto pair = compare(a, b, config);
  check(a.metadata_calls == 1 && b.metadata_calls == 1 && a.properties_calls == 1 && b.properties_calls == 1,
    "each provider called once for a paired run");
  check(a.calls == 2 && b.calls == 2 && pair.reference.samples.size() == 2 && pair.contender.samples.size() == 2,
    "explicit configuration wins over both case bands");
  check(pair.reference.properties.band == Band::smoke && pair.contender.properties.band == Band::deep
    && pair.contender.policy.band == Band::inherit && pair.reference.policy.target_batch == 1ns,
    "requested bands and the actual common policy are recorded");
  check(std::get<std::uint64_t>(pair.reference.metadata[0].value) == 128
    && pair.contender.metadata[0].name == "input bytes", "typed metadata carried independently");
  check(matches(pair.reference.properties, {"format", {"flat-zinc", "small"}, Band::smoke})
    && !matches(pair.reference.properties, {"format", {"unknown"}, Band::smoke})
    && !matches(pair.reference.properties, {"another group", {}, Band::smoke}),
    "group and all tags constrain selection");
  std::ostringstream report;
  write_ndjson(report, "baseline", "contender", pair);
  check(report.str().contains("\"effective_band\":\"explicit\"") && report.str().contains("\"name\":\"input bytes\",\"value\":128"),
    "NDJSON retains the selected sampling policy and metadata types");

  auto work = with_properties([] { return 7; }, {"quick", {"small"}, Band::smoke});
  const auto quick = measure(work);
  check(quick.policy.band == Band::smoke && quick.samples.size() == configuration_for(Band::smoke).samples,
    "a callable band changes real collection when configuration is omitted");
}

}  // namespace

int main() {
  statistics();
  sampling();
  memory_telemetry();
  descriptions();
  return ycxx::unit::result("bench");
}
