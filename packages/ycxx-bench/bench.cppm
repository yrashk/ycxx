// Adapted from Flyology Bench. See LICENSE-FLYOLOGY.
export module ycxx.bench;
import std;

export namespace ycxx::bench {

using Nanoseconds = std::chrono::nanoseconds;
using Iterations = std::size_t;

enum class Phase { warming, calibrating, sampling, analyzing };
enum class Batching { equal_time, shared_iterations };
enum class Verdict { unavailable, contender_faster, reference_faster, equivalent, inconclusive };
enum class Style { plain, ansi };
enum class Band { inherit, smoke, standard, deep };

struct Sample;

struct Progress {
  Phase phase;
  std::size_t completed = 0;
  std::size_t total = 0;
};

// Attach by reference to Configuration::progress when out is an interactive
// terminal. Clear the three-line display before printing the final report.
class TerminalProgress {
public:
  TerminalProgress(std::ostream& out, std::size_t cases, Band profile);
  ~TerminalProgress();
  TerminalProgress(const TerminalProgress&) = delete;
  TerminalProgress& operator=(const TerminalProgress&) = delete;

  void operator()(Progress);
  void clear();

private:
  using Clock = std::chrono::steady_clock;
  std::ostream& out_;
  Clock::time_point started_ = Clock::now();
  Clock::time_point last_{};
  std::optional<Phase> phase_;
  std::size_t completed_ = 0;
  std::size_t total_ = 0;
  std::size_t cases_;
  Band profile_;
  bool visible_ = false;
};

struct Configuration {
  Band profile = Band::inherit;
  Nanoseconds warmup_time = std::chrono::milliseconds(100);
  Nanoseconds measurement_time = std::chrono::milliseconds(500);
  Nanoseconds minimum_sample_time = std::chrono::microseconds(100);
  Nanoseconds maximum_sampling_time{};
  std::size_t samples = 50;
  Iterations maximum_iterations = 1u << 30;
  Batching batching = Batching::equal_time;
  double practical_threshold = 0.01;
  double confidence = 0.95;
  std::size_t bootstrap_resamples = 2'000;
  std::uint64_t seed = 1;
  bool subtract_timer_cost = false;
  bool process_telemetry = false;
  std::function<void(Progress)> progress;
  // Called after timing and teardown. Case and round indices are zero-based.
  std::function<void(std::size_t, std::size_t, const Sample&)> sample_observer;
};

Configuration configuration_for(Band);
std::string_view band_name(Band);

struct Properties {
  std::string group;
  std::vector<std::string> tags;
  Band band = Band::inherit;
};

struct Selection {
  std::string_view group;
  std::vector<std::string_view> tags;
  std::optional<Band> band;
};

bool matches(const Properties&, const Selection&);

template<class Callable>
concept HasBenchmarkProperties = requires(const std::remove_reference_t<Callable>& callable) {
  { callable.benchmark_properties() } -> std::convertible_to<Properties>;
};

struct ClockInfo {
  std::string_view source;
  double nominal_ns;
  Nanoseconds observed{};
  Nanoseconds median_read{};
  Nanoseconds minimum_read{};
};

struct HostInfo {
  std::string system;
  std::string release;
  std::string machine;
  unsigned logical_cpus = 0;
};

enum class RssSource { unspecified, linux_proc_status, mach_task_basic_info };

struct ProcessSample {
  Nanoseconds cpu_time{};
  std::optional<std::uint64_t> peak_rss_bytes;
  std::optional<Nanoseconds> thread_cpu_time;
  std::optional<std::uint64_t> rss_before_bytes;
  std::optional<std::uint64_t> rss_after_bytes;
  std::uint64_t minor_faults = 0;
  std::uint64_t major_faults = 0;
  std::uint64_t voluntary_switches = 0;
  std::uint64_t involuntary_switches = 0;
  std::uint64_t block_input_operations = 0;
  std::uint64_t block_output_operations = 0;
  std::optional<double> host_cpu_busy_fraction;
  RssSource rss_source = RssSource::unspecified;
  // Platform resource-usage counter, retained separately from the coherent
  // resident-memory snapshot. On Linux this can retain pre-exec history.
  std::optional<std::uint64_t> rusage_max_rss_bytes;
};

struct Sample {
  Iterations iterations;
  Nanoseconds elapsed;
  std::optional<ProcessSample> process;
  double ns_per_operation(Nanoseconds timer_cost = {}) const
    pre(iterations > 0 && elapsed.count() >= 0 && timer_cost.count() >= 0) {
    return double((elapsed - std::min(timer_cost, elapsed)).count()) / double(iterations);
  }
};

struct Interval {
  double low;
  double high;
};

struct Summary {
  double minimum = 0;
  double median = 0;
  double mean = 0;
  double maximum = 0;
  double p95 = 0;
  double p99 = 0;
  double mad = 0;
  double coefficient_of_variation = 0;
  double lag_one = 0;
  std::size_t mild_outliers = 0;
  std::size_t severe_outliers = 0;
  std::optional<Interval> mean_interval;
};

using MetadataValue = std::variant<std::string, std::int64_t, std::uint64_t, double, bool>;

struct MetadataItem {
  std::string name;
  MetadataValue value;
};
using Metadata = std::vector<MetadataItem>;

template<class Callable>
concept HasBenchmarkMetadata = requires(const std::remove_reference_t<Callable>& callable) {
  { callable.benchmark_metadata() } -> std::convertible_to<Metadata>;
};

struct SamplingPolicy {
  Band band = Band::inherit;
  Nanoseconds warmup{};
  Nanoseconds target_batch{};
  Nanoseconds collection_limit{};
  std::size_t requested_samples = 0;
  std::size_t bootstrap_resamples = 0;
  std::uint64_t seed = 1;
  Batching batching = Batching::equal_time;
};

struct Measurement {
  Properties properties;
  Metadata metadata;
  SamplingPolicy policy;
  ClockInfo clock{};
  HostInfo host;
  std::vector<Sample> samples;
  Summary summary;
  Nanoseconds timer_cost{};
  bool sampling_limited = false;
  double confidence = 0.95;
  double clock_floor_ns_per_operation() const;
};

struct PairedSummary {
  double speedup = 0;
  double relative_time_change = 0;
  double lag_one = 0;
  std::optional<Interval> speedup_interval;
  Verdict verdict = Verdict::unavailable;
};

struct Comparison {
  Measurement reference;
  Measurement contender;
  PairedSummary paired;
  bool contender_first_in_round_zero = false;
  double practical_threshold = 0.01;
};

struct Shootout {
  std::vector<Measurement> cases;
  // Cases are paired with case 0 by sample index. Case 0 is the reference.
  std::vector<PairedSummary> paired;
  std::size_t first_case_in_round_zero = 0;
  double practical_threshold = 0.01;
};

// Analyze external recordings using the same estimators as the in-process runner.
Summary summarize(std::span<const Sample>, const Configuration& = {}, Nanoseconds timer_cost = {});
PairedSummary summarize_paired(std::span<const Sample> reference, std::span<const Sample> contender,
                               const Configuration& = {}, Nanoseconds timer_cost = {});
void write_console(std::ostream&, std::string_view name, const Measurement&, Style = Style::plain);
void write_console(std::ostream&, std::string_view reference, std::string_view contender,
                   const Comparison&, Style = Style::plain);
void write_console(std::ostream&, std::span<const std::string_view> names, const Shootout&,
                   Style = Style::plain);
void write_ndjson(std::ostream&, std::string_view name, const Measurement&);
void write_ndjson(std::ostream&, std::string_view reference, std::string_view contender, const Comparison&);
bool stdout_is_terminal();
void write_ndjson(std::ostream&, std::span<const std::string_view> names, const Shootout&);
void write_csv(std::ostream&, std::string_view name, const Measurement&);

// Line-oriented events for interactive machine consumers. Each call flushes its record.
void write_live_start(std::ostream&, std::span<const std::string_view> names);
void write_live_progress(std::ostream&, Progress);
void write_live_sample(std::ostream&, std::string_view name, std::size_t round, const Sample&);

// Use an observable, varying input for pure operations. Escape a result before the
// end timestamp. For a batch, return a value derived from all of its operations.
template<class T>
void do_not_optimize(const T& value) {
  if constexpr ((std::is_arithmetic_v<T> || std::is_pointer_v<T>) && sizeof(T) <= sizeof(void*))
    asm volatile("" : : "g"(value) : "memory");
  else
    asm volatile("" : : "g"(std::addressof(value)) : "memory");
}

inline void clobber_memory() { asm volatile("" : : : "memory"); }

}  // namespace ycxx::bench

namespace ycxx::bench::detail {

Nanoseconds now_ns();
ClockInfo clock_info();
HostInfo host_info();
struct HostTicks {
  std::uint64_t busy;
  std::uint64_t total;
};
struct ResidentMemory {
  std::uint64_t rss_bytes;
  std::uint64_t peak_bytes;
  RssSource source;
};
struct ProcessCounters {
  Nanoseconds cpu_time;
  std::optional<std::uint64_t> rusage_max_rss_bytes;
  std::optional<Nanoseconds> thread_cpu_time;
  std::optional<ResidentMemory> memory;
  std::uint64_t minor_faults;
  std::uint64_t major_faults;
  std::uint64_t voluntary_switches;
  std::uint64_t involuntary_switches;
  std::uint64_t block_input_operations;
  std::uint64_t block_output_operations;
  std::optional<HostTicks> host;
};
std::optional<ProcessCounters> process_info(bool starting);
std::optional<HostTicks> host_cpu_ticks();
ProcessSample process_sample(const ProcessCounters&, const ProcessCounters&);
void validate(const Configuration&);

struct Noop {
  void operator()() const {}
};

struct Description {
  Properties properties;
  Metadata metadata;
};

template<class Work>
Description describe(const Work& work) {
  Description description;
  if constexpr (HasBenchmarkProperties<Work>) description.properties = work.benchmark_properties();
  if constexpr (HasBenchmarkMetadata<Work>) description.metadata = work.benchmark_metadata();
  return description;
}

template<class Work>
struct DescribedWork {
  Work work;
  Description description;

  const Metadata& benchmark_metadata() const { return description.metadata; }
  const Properties& benchmark_properties() const { return description.properties; }

  template<class... Args>
    requires std::invocable<Work&, Args...>
  decltype(auto) operator()(Args&&... args) {
    return std::invoke(work, std::forward<Args>(args)...);
  }
};

template<class Work>
auto described(Work&& work, Description description) {
  return DescribedWork<std::decay_t<Work>>{std::forward<Work>(work), std::move(description)};
}

template<class Work, class... Args>
void consume(Work& work, Args... args) {
  if constexpr (std::is_void_v<std::invoke_result_t<Work&, Args...>>) std::invoke(work, args...);
  else do_not_optimize(std::invoke(work, args...));
}

template<class Batch>
Sample time_batch(Batch& batch, std::size_t which, Iterations n, bool telemetry) {
  const auto before = telemetry ? process_info(true) : std::nullopt;
  auto started = now_ns();
  clobber_memory();
  consume(batch, which, n);
  clobber_memory();
  Sample sample{n, now_ns() - started, {}};
  if (before) {
    if (const auto after = process_info(false)) sample.process = process_sample(*before, *after);
  }
  return sample;
}

inline void notify(const Configuration& config, Progress progress) {
  if (config.progress) config.progress(progress);
}

inline Iterations grow(Iterations count, Iterations limit) {
  return count + std::min(count, limit - count);
}

template<class Batch, class Setup, class Teardown>
std::vector<Measurement> collect(std::vector<Description> descriptions, Batch& batch,
                                 const std::optional<Configuration>& options, Setup& setup, Teardown& teardown,
                                 Configuration* used_configuration = nullptr) {
  const auto cases = descriptions.size();
  auto band = Band::inherit;
  for (const auto& description : descriptions) band = std::max(band, description.properties.band);
  if (band == Band::inherit) band = Band::standard;
  const auto config = options ? *options : configuration_for(band);
  validate(config);
  if (used_configuration) *used_configuration = config;
  const auto clock = clock_info();
  const auto host = host_info();
  const auto target = std::max(config.minimum_sample_time,
    config.measurement_time / static_cast<Nanoseconds::rep>(config.samples));
  const auto limit = config.maximum_iterations;
  std::vector<Iterations> iterations(cases, 1);
  std::vector<Measurement> results(cases);
  for (std::size_t i = 0; i != cases; ++i) {
    auto& result = results[i];
    result.properties = std::move(descriptions[i].properties);
    result.metadata = std::move(descriptions[i].metadata);
    result.policy = {config.profile, config.warmup_time, target, config.maximum_sampling_time,
                     config.samples, config.bootstrap_resamples, config.seed, config.batching};
    result.clock = clock;
    result.host = host;
    result.confidence = config.confidence;
    result.samples.reserve(config.samples);
    if (config.subtract_timer_cost) result.timer_cost = clock.minimum_read;
  }

  const auto timed = [&](std::size_t which, Iterations count, bool telemetry) {
    std::invoke(setup, which);
    Sample sample;
    try {
      sample = time_batch(batch, which, count, telemetry);
    } catch (...) {
      std::invoke(teardown, which);
      throw;
    }
    std::invoke(teardown, which);
    return sample;
  };

  const auto position = [&](std::size_t round, std::size_t index) {
    return (config.seed % cases + round % cases + index) % cases;
  };
  if (config.warmup_time.count() > 0) {
    const auto start = now_ns();
    notify(config, {Phase::warming, 0, 100});
    for (std::size_t round = 0;; ++round) {
      for (std::size_t i = 0; i != cases; ++i) {
        const auto which = position(round, i);
        const auto sample = timed(which, iterations[which], false);
        if (sample.elapsed < target / 2) iterations[which] = grow(iterations[which], limit);
      }
      auto elapsed = now_ns() - start;
      const auto fraction = double(elapsed.count()) / double(config.warmup_time.count());
      notify(config, {Phase::warming, std::size_t(100 * std::min(1.0, fraction)), 100});
      if (elapsed >= config.warmup_time) break;
    }
  }

  notify(config, {Phase::calibrating});
  for (std::size_t which = 0; which != cases; ++which) {
    auto& count = iterations[which];
    for (unsigned hits = 0; hits < 3 && count < limit;) {
      if (timed(which, count, false).elapsed >= target - target / 10) ++hits;
      else {
        count = grow(count, limit);
        hits = 0;
      }
    }
  }
  if (config.batching == Batching::shared_iterations) {
    const auto [minimum, maximum] = std::ranges::minmax_element(iterations);
    // Bring the fastest case up to target without requiring a much slower case
    // to run for more than eight of its own calibrated batches.
    auto shared = *minimum;
    for (unsigned i = 0; i != 3; ++i) shared = grow(shared, *maximum);
    std::ranges::fill(iterations, shared);
  }

  notify(config, {Phase::sampling, 0, config.samples});
  auto sampling_start = now_ns();
  for (std::size_t round = 0; round != config.samples; ++round) {
    for (std::size_t i = 0; i != cases; ++i) {
      const auto which = position(round, i);
      auto& samples = results[which].samples;
      samples.push_back(timed(which, iterations[which], config.process_telemetry));
      if (config.sample_observer) config.sample_observer(which, round, samples.back());
    }
    notify(config, {Phase::sampling, round + 1, config.samples});
    if (round + 1 < config.samples && round + 1 >= 10 && config.maximum_sampling_time.count() > 0
        && now_ns() - sampling_start >= config.maximum_sampling_time) {
      for (auto& result : results) result.sampling_limited = true;
      break;
    }
  }

  notify(config, {Phase::analyzing});
  for (auto& result : results) result.summary = summarize(result.samples, config, result.timer_cost);
  return results;
}

}  // namespace ycxx::bench::detail

export namespace ycxx::bench {

template<class Work>
auto with_metadata(Work&& work, Metadata metadata) {
  Properties properties;
  if constexpr (HasBenchmarkProperties<Work>) properties = std::as_const(work).benchmark_properties();
  return detail::described(std::forward<Work>(work), {std::move(properties), std::move(metadata)});
}

template<class Work>
auto with_properties(Work&& work, Properties properties, Metadata metadata = {}) {
  if constexpr (HasBenchmarkMetadata<Work>) {
    if (metadata.empty()) metadata = std::as_const(work).benchmark_metadata();
  }
  return detail::described(std::forward<Work>(work), {std::move(properties), std::move(metadata)});
}

template<class Batch, class Setup = detail::Noop, class Teardown = detail::Noop>
  requires std::invocable<Batch&, Iterations> && std::invocable<Setup&> && std::invocable<Teardown&>
Measurement measure_batched(Batch&& batch, const std::optional<Configuration>& config = {},
                            Setup&& setup = {}, Teardown&& teardown = {}) {
  auto work = [&](std::size_t, Iterations n) -> decltype(auto) { return std::invoke(batch, n); };
  auto prepare = [&](std::size_t) { std::invoke(setup); };
  auto finish = [&](std::size_t) { std::invoke(teardown); };
  std::vector<detail::Description> description;
  description.push_back(detail::describe(batch));
  return std::move(detail::collect(std::move(description), work, config, prepare, finish).front());
}

template<class Work>
  requires std::invocable<Work&>
Measurement measure(Work&& operation, const std::optional<Configuration>& config = {}) {
  auto batch = [&](Iterations n) {
    for (Iterations i = 0; i != n; ++i) detail::consume(operation);
  };
  if constexpr (HasBenchmarkMetadata<Work> || HasBenchmarkProperties<Work>)
    return measure_batched(detail::described(batch, detail::describe(operation)), config);
  else return measure_batched(batch, config);
}

template<class Batch, class Setup = detail::Noop, class Teardown = detail::Noop>
  requires std::invocable<Batch&, std::size_t, Iterations>
    && (std::invocable<Setup&, std::size_t> || std::invocable<Setup&>)
    && (std::invocable<Teardown&, std::size_t> || std::invocable<Teardown&>)
Shootout compare_many_batched(std::size_t cases, Batch&& batch,
                              const std::optional<Configuration>& options = {},
                              Setup&& setup = {}, Teardown&& teardown = {})
  pre(cases >= 2 && cases <= 16) {
  auto prepare = [&](std::size_t index) {
    if constexpr (std::invocable<Setup&, std::size_t>) std::invoke(setup, index);
    else std::invoke(setup);
  };
  auto finish = [&](std::size_t index) {
    if constexpr (std::invocable<Teardown&, std::size_t>) std::invoke(teardown, index);
    else std::invoke(teardown);
  };
  std::vector<detail::Description> descriptions(cases, detail::describe(batch));
  for (std::size_t i = 0; i != cases; ++i) {
    if constexpr (requires(const std::remove_reference_t<Batch>& work) {
      { work.benchmark_properties(i) } -> std::convertible_to<Properties>;
    }) descriptions[i].properties = std::as_const(batch).benchmark_properties(i);
    if constexpr (requires(const std::remove_reference_t<Batch>& work) {
      { work.benchmark_metadata(i) } -> std::convertible_to<Metadata>;
    }) descriptions[i].metadata = std::as_const(batch).benchmark_metadata(i);
  }
  Shootout result;
  Configuration config;
  result.cases = detail::collect(std::move(descriptions), batch, options, prepare, finish, &config);
  result.first_case_in_round_zero = config.seed % cases;
  result.practical_threshold = config.practical_threshold;
  for (const auto& contender : result.cases) {
    const auto& reference = result.cases.front();
    result.paired.push_back(summarize_paired(reference.samples, contender.samples, config, reference.timer_cost));
  }
  return result;
}

template<class Reference, class Contender>
  requires std::invocable<Reference&, Iterations> && std::invocable<Contender&, Iterations>
Comparison compare_batched(Reference&& reference, Contender&& contender,
                           const std::optional<Configuration>& options = {}) {
  std::vector<detail::Description> descriptions;
  descriptions.push_back(detail::describe(reference));
  descriptions.push_back(detail::describe(contender));
  auto batch = [&](std::size_t which, Iterations n) {
    if (which == 0) detail::consume(reference, n);
    else detail::consume(contender, n);
  };
  auto noop = [](std::size_t) {};
  Configuration config;
  auto result = detail::collect(std::move(descriptions), batch, options, noop, noop, &config);
  auto paired = summarize_paired(result[0].samples, result[1].samples, config, result[0].timer_cost);
  return {std::move(result[0]), std::move(result[1]), paired, config.seed % 2 == 1, config.practical_threshold};
}

template<class Reference, class Contender>
  requires std::invocable<Reference&> && std::invocable<Contender&>
Comparison compare(Reference&& reference, Contender&& contender,
                   const std::optional<Configuration>& config = {}) {
  auto a = detail::described([&](Iterations n) {
    for (Iterations i = 0; i != n; ++i) detail::consume(reference);
  }, detail::describe(reference));
  auto b = detail::described([&](Iterations n) {
    for (Iterations i = 0; i != n; ++i) detail::consume(contender);
  }, detail::describe(contender));
  return compare_batched(a, b, config);
}

}  // namespace ycxx::bench
