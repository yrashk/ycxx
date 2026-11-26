// Sweep, retained recording, and independent comparison designs adapted from
// Flyology Bench. See LICENSE-FLYOLOGY.
export module ycxx.bench.experiments;
import std;
export import ycxx.bench;

namespace ycxx::bench::experiments::detail {

// SplitMix64 gives reproducible resampling without depending on a library RNG.
std::uint64_t random(std::uint64_t& state) {
  auto value = (state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

std::uint64_t index(std::uint64_t& state, std::uint64_t limit)
  pre(limit > 0) {
  const auto threshold = -limit % limit;
  for (auto draw = random(state);; draw = random(state)) {
    if (draw >= threshold) return draw % limit;
  }
}

}  // namespace ycxx::bench::experiments::detail

export namespace ycxx::bench::experiments {

enum class Parameter { size, count };

struct SweepPoint {
  Parameter parameter;
  std::uint64_t value;
  std::string label;
};

struct Work {
  std::uint64_t units;
  std::string unit;
};

struct Rate {
  double operations_per_second;
  double units_per_second;
  std::optional<Interval> units_per_second_interval;
};

inline std::optional<Rate> throughput(const Measurement& measurement, const Work& work) {
  const auto mean = measurement.summary.mean;
  if (work.units == 0 || measurement.samples.empty() || !(mean > 0)) return {};
  const double operations = 1e9 / mean;
  const double units_per_second = operations * double(work.units);
  if (!std::isfinite(operations) || !std::isfinite(units_per_second)) return {};
  Rate rate{operations, units_per_second, {}};
  const auto interval = measurement.summary.mean_interval;
  if (interval && interval->low > 0 && interval->high >= interval->low) {
    const auto scale = 1e9 * double(work.units);
    const auto lower = scale / interval->high;
    const auto upper = scale / interval->low;
    if (std::isfinite(lower) && std::isfinite(upper))
      rate.units_per_second_interval = Interval{lower, upper};
  }
  return rate;
}

enum class SweepStatus { measured, skipped, invalid_work, failed };
enum class SweepError { zero_parameter, repeated_point };

template<class Result>
struct SweepRow {
  SweepPoint point;
  SweepStatus status = SweepStatus::skipped;
  std::optional<Work> work;
  std::optional<Result> result;
  std::optional<Rate> reference_rate;
  std::optional<Rate> contender_rate;
  std::string diagnostic;
};

template<class Result>
concept SweepResult = std::same_as<Result, Measurement> || std::same_as<Result, Comparison>;

// Selection and work identity are evaluated before each measurement. The
// runner itself may use a statically bound measure or compare operation.
template<class Select, class Run>
  requires std::invocable<Select&, const SweepPoint&>
    && std::same_as<std::invoke_result_t<Select&, const SweepPoint&>, std::optional<Work>>
    && std::invocable<Run&, const SweepPoint&, const Work&>
    && SweepResult<std::invoke_result_t<Run&, const SweepPoint&, const Work&>>
auto sweep(std::span<const SweepPoint> points, Select&& select_work, Run&& run) {
  using Result = std::invoke_result_t<Run&, const SweepPoint&, const Work&>;
  using Sweep = std::expected<std::vector<SweepRow<Result>>, SweepError>;
  std::set<std::pair<Parameter, std::uint64_t>> identities;
  for (const auto& point : points) {
    if (point.value == 0) return Sweep(std::unexpected(SweepError::zero_parameter));
    if (!identities.emplace(point.parameter, point.value).second)
      return Sweep(std::unexpected(SweepError::repeated_point));
  }
  std::vector<SweepRow<Result>> rows;
  rows.reserve(points.size());
  for (const auto& point : points) {
    auto& row = rows.emplace_back();
    row.point = point;
    try {
      auto work = std::invoke(select_work, point);
      if (!work) continue;
      if (work->units == 0 || work->unit.empty()) {
        row.status = SweepStatus::invalid_work;
        continue;
      }
      row.work = std::move(*work);
      row.result = std::invoke(run, point, *row.work);
      if constexpr (std::same_as<Result, Measurement>) row.reference_rate = throughput(*row.result, *row.work);
      else {
        row.reference_rate = throughput(row.result->reference, *row.work);
        row.contender_rate = throughput(row.result->contender, *row.work);
      }
      row.status = SweepStatus::measured;
    } catch (const std::exception& failure) {
      row.status = SweepStatus::failed;
      row.diagnostic = failure.what();
    }
  }
  return Sweep(std::move(rows));
}

enum class Trend { constant, logarithmic, linear, linearithmic, quadratic, cubic };
enum class ScalingStatus { observed, insufficient_points, invalid_points, short_range, poor_fit, ambiguous };

struct ScalingPoint {
  Parameter parameter;
  std::uint64_t value;
  double ns_per_operation;
};

struct ScalingFit {
  Trend trend;
  double multiplier;
  double rms_log_error;
  double max_log_error;
  double r_squared;
};

struct ScalingAnalysis {
  ScalingStatus status = ScalingStatus::insufficient_points;
  std::optional<Parameter> parameter;
  std::uint64_t minimum = 0;
  std::uint64_t maximum = 0;
  std::vector<ScalingFit> fits;
  std::optional<Trend> observed;
};

// Fit fixed growth shapes in log space over the supplied range. The winning
// shape is a description of these observations, not a complexity bound.
ScalingAnalysis analyze_scaling(std::span<const ScalingPoint> points) {
  ScalingAnalysis result;
  if (points.empty()) return result;
  result.parameter = points.front().parameter;
  result.minimum = points.front().value;
  result.maximum = points.front().value;
  std::set<std::uint64_t> identities;
  std::vector<double> measured;
  double mean = 0;
  bool invalid = false;
  for (const auto& point : points) {
    result.minimum = std::min(result.minimum, point.value);
    result.maximum = std::max(result.maximum, point.value);
    if (point.parameter != result.parameter) {
      result.parameter.reset();
      invalid = true;
    }
    if (point.value == 0 || !identities.insert(point.value).second
        || !(point.ns_per_operation > 0) || !std::isfinite(point.ns_per_operation)) {
      invalid = true;
      continue;
    }
    measured.push_back(std::log(point.ns_per_operation));
    mean += measured.back();
  }
  if (invalid) {
    result.status = ScalingStatus::invalid_points;
    return result;
  }
  if (points.size() < 4) return result;
  if (result.minimum > result.maximum / 2) {
    result.status = ScalingStatus::short_range;
    return result;
  }
  mean /= measured.size();
  double variance = 0;
  for (const auto log_measured : measured) variance += (log_measured - mean) * (log_measured - mean);

  constexpr Trend candidates[] = {Trend::constant, Trend::logarithmic, Trend::linear, Trend::linearithmic,
                                  Trend::quadratic, Trend::cubic};
  for (const auto trend : candidates) {
    std::vector<double> offsets;
    offsets.reserve(points.size());
    double multiplier = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const auto n = double(points[i].value);
      const auto log_n = std::log(n);
      const double basis[] = {0, std::log(std::log1p(n)), log_n, log_n + std::log(std::log1p(n)),
                              2 * log_n, 3 * log_n};
      offsets.push_back(measured[i] - basis[std::size_t(trend)]);
      multiplier += offsets.back();
    }
    multiplier /= offsets.size();
    double sum_squared = 0;
    double maximum = 0;
    for (const auto offset : offsets) {
      const auto error = offset - multiplier;
      sum_squared += error * error;
      maximum = std::max(maximum, std::abs(error));
    }
    const auto r_squared = variance > 0 ? 1 - sum_squared / variance : double(sum_squared == 0);
    result.fits.push_back({trend, std::exp(multiplier), std::sqrt(sum_squared / offsets.size()), maximum, r_squared});
  }
  std::ranges::sort(result.fits, [](const auto& a, const auto& b) {
    return std::pair{a.rms_log_error, a.trend} < std::pair{b.rms_log_error, b.trend};
  });
  if (result.fits[0].rms_log_error > 0.20) result.status = ScalingStatus::poor_fit;
  else if (result.fits[1].rms_log_error - result.fits[0].rms_log_error < 0.025)
    result.status = ScalingStatus::ambiguous;
  else {
    result.status = ScalingStatus::observed;
    result.observed = result.fits.front().trend;
  }
  return result;
}

enum class Outcome { success, failure, timeout, cancelled, abandoned };
enum class Retention { reservoir, first, latest };

struct Observation {
  std::uint64_t sequence;
  Outcome outcome;
  Sample sample;
};

struct Recording {
  std::string identity;
  Retention retention;
  std::size_t capacity;
  std::uint64_t observed;
  std::uint64_t active;
  std::array<std::uint64_t, 5> counts;
  std::vector<Observation> retained;

  std::uint64_t omitted() const { return observed - retained.size(); }

  std::vector<Sample> samples(std::optional<Outcome> outcome = {}) const {
    std::vector<Sample> result;
    result.reserve(retained.size());
    for (const auto& observation : retained) {
      if (!outcome || observation.outcome == *outcome) result.push_back(observation.sample);
    }
    return result;
  }
};

// The recorder owns its identity and retention capacity. The end timestamp is
// read before the sample-store lock. Keep this object alive across every Span.
class Recorder {
  using Clock = std::chrono::steady_clock;

public:
  class Span {
  public:
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span& operator=(Span&&) = delete;

    Span(Span&& other) noexcept
      : recorder_(std::exchange(other.recorder_, nullptr)), started_(other.started_), iterations_(other.iterations_) {}

    ~Span() {
      if (!recorder_) return;
      try {
        finish(Outcome::abandoned);
      } catch (...) {
        // A failed native mutex is unrecoverable for a disappearing span.
      }
    }

    void finish(Outcome outcome, std::optional<ProcessSample> process = {}) {
      const auto ended = Clock::now();
      if (auto* recorder = std::exchange(recorder_, nullptr))
        recorder->retain(Sample{iterations_, std::chrono::duration_cast<Nanoseconds>(ended - started_),
          std::move(process)}, outcome, true);
    }

  private:
    friend class Recorder;
    Span(Recorder& recorder, Iterations iterations)
      : recorder_(&recorder), started_(Clock::now()), iterations_(iterations) {}

    Recorder* recorder_;
    Clock::time_point started_;
    Iterations iterations_;
  };

  explicit Recorder(std::string identity, std::size_t capacity,
                    Retention retention = Retention::reservoir, std::uint64_t seed = 1)
    pre(!identity.empty()) : identity_(std::move(identity)), capacity_(capacity), retention_(retention), seed_(seed) {
    retained_.reserve(capacity_);
  }

  Span begin(Iterations iterations = 1) pre(iterations > 0) {
    {
      std::lock_guard lock(mutex_);
      ++active_;
    }
    return Span(*this, iterations);
  }

  void record(Nanoseconds elapsed, Outcome outcome, Iterations iterations = 1,
              std::optional<ProcessSample> process = {})
    pre(elapsed.count() >= 0 && iterations > 0) {
    retain(Sample{iterations, elapsed, std::move(process)}, outcome, false);
  }

  Recording snapshot() const {
    auto result = [&] {
      std::lock_guard lock(mutex_);
      return Recording{identity_, retention_, capacity_, observed_, active_, counts_, retained_};
    }();
    std::ranges::sort(result.retained, {}, &Observation::sequence);
    return result;
  }

private:
  void retain(Sample sample, Outcome outcome, bool from_span) {
    std::lock_guard lock(mutex_);
    contract_assert(observed_ < std::numeric_limits<std::uint64_t>::max());
    contract_assert(std::size_t(outcome) < counts_.size());
    if (from_span) {
      contract_assert(active_ > 0);
      --active_;
    }
    ++counts_[std::size_t(outcome)];
    const Observation observation{++observed_, outcome, std::move(sample)};
    if (retained_.size() < capacity_) retained_.push_back(observation);
    else if (capacity_ > 0) {
      if (retention_ == Retention::latest) retained_[(observed_ - 1) % capacity_] = observation;
      else if (retention_ == Retention::reservoir) {
        const auto slot = detail::index(seed_, observed_);
        if (slot < capacity_) retained_[slot] = observation;
      }
    }
  }

  std::string identity_;
  std::size_t capacity_;
  Retention retention_;
  std::uint64_t seed_;
  mutable std::mutex mutex_;
  std::uint64_t observed_ = 0;
  std::uint64_t active_ = 0;
  std::array<std::uint64_t, 5> counts_{};
  std::vector<Observation> retained_;
};

struct IndependentSummary {
  double speedup = 0;
  double relative_time_change = 0;
  std::optional<Interval> speedup_interval;
  Verdict verdict = Verdict::unavailable;
};

// These are separate runs, with no round pairing. Each bootstrap draw
// resamples circular blocks from the two distributions independently.
// BOOTSTRAP: contracts: keep this function out of line because GCC 16.2 can
// omit its TU-local violation handler when an importer inlines a contract.
IndependentSummary compare_independent(std::span<const Sample> reference, std::span<const Sample> contender,
                                               const Configuration& config = {})
  pre(config.confidence > 0 && config.confidence < 1)
  pre(config.practical_threshold >= 0 && config.practical_threshold < 1) {
  IndependentSummary result;
  if (reference.empty() || contender.empty()) return result;
  const auto logs = [](std::span<const Sample> samples) {
    std::vector<double> values;
    values.reserve(samples.size());
    for (const auto& sample : samples) {
      const double elapsed = sample.ns_per_operation();
      if (!(elapsed > 0)) return std::vector<double>{};
      values.push_back(std::log(elapsed));
    }
    return values;
  };
  auto a = logs(reference);
  auto b = logs(contender);
  if (a.empty() || b.empty()) return result;
  const auto mean = [](std::span<const double> values) {
    double sum = 0;
    for (const auto value : values) sum += value;
    return sum / values.size();
  };
  const auto log_speedup = mean(a) - mean(b);
  result.speedup = std::exp(log_speedup);
  result.relative_time_change = std::expm1(-log_speedup);
  if (a.size() < 10 || b.size() < 10 || config.bootstrap_resamples == 0) return result;

  const auto draw_mean = [](std::span<const double> values, std::uint64_t& seed) {
    const auto width = std::size_t(std::ceil(std::sqrt(double(values.size()))));
    double sum = 0;
    for (std::size_t count = 0; count != values.size();) {
      const auto start = detail::index(seed, values.size());
      for (std::size_t j = 0; j != width && count != values.size(); ++count, ++j)
        sum += values[(start + j) % values.size()];
    }
    return sum / values.size();
  };
  std::uint64_t reference_seed = config.seed ^ 0x94d049bb133111ebULL;
  std::uint64_t contender_seed = config.seed ^ 0xd1b54a32d192ed03ULL;
  std::vector<double> estimates;
  estimates.reserve(config.bootstrap_resamples);
  for (std::size_t draw = 0; draw != config.bootstrap_resamples; ++draw) {
    const auto reference_mean = draw_mean(a, reference_seed);
    const auto contender_mean = draw_mean(b, contender_seed);
    estimates.push_back(reference_mean - contender_mean);
  }
  std::ranges::sort(estimates);
  const auto percentile = [&](double fraction) {
    const auto rank = (estimates.size() - 1) * fraction;
    const auto lower = std::size_t(rank);
    const auto upper = std::min(lower + 1, estimates.size() - 1);
    return estimates[lower] + (estimates[upper] - estimates[lower]) * (rank - lower);
  };
  const double tail = (1 - config.confidence) / 2;
  const auto low = percentile(tail);
  const auto high = percentile(1 - tail);
  result.speedup_interval = Interval{std::exp(low), std::exp(high)};
  const auto fastest = std::expm1(-high);
  const auto slowest = std::expm1(-low);
  if (slowest < -config.practical_threshold) result.verdict = Verdict::contender_faster;
  else if (fastest > config.practical_threshold) result.verdict = Verdict::reference_faster;
  else if (fastest >= -config.practical_threshold && slowest <= config.practical_threshold)
    result.verdict = Verdict::equivalent;
  else result.verdict = Verdict::inconclusive;
  return result;
}

}  // namespace ycxx::bench::experiments
