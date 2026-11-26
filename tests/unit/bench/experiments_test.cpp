import std;
import ycxx.bench.experiments;
import ycxx.unit;

using namespace ycxx::bench;
using namespace ycxx::bench::experiments;
using ycxx::unit::check;

namespace {

using namespace std::chrono_literals;

Sample sample(Nanoseconds elapsed, Iterations iterations = 1) {
  Sample result{};
  result.elapsed = elapsed;
  result.iterations = iterations;
  return result;
}

void sweeps() {
  const SweepPoint points[] = {
    {Parameter::size, 10, "first"},
    {Parameter::size, 20, "skipped"},
    {Parameter::count, 10, "failure"},
    {Parameter::count, 12, "zero work"},
  };
  unsigned runs = 0;
  const auto select = [](const SweepPoint& point) -> std::optional<Work> {
    if (point.value == 20) return {};
    if (point.value == 12) return Work{0, "bytes"};
    return Work{point.value * 2, "bytes"};
  };
  const auto run = [&](const SweepPoint& point, const Work&) {
    ++runs;
    if (point.parameter == Parameter::count) throw std::runtime_error("fixture failure");
    Measurement m;
    m.samples = {sample(30ns, 3)};
    m.summary.mean = 10;
    m.summary.mean_interval = Interval{8, 12};
    return m;
  };
  const auto result = sweep(points, select, run);
  check(result.has_value() && result->size() == 4 && runs == 2, "measure only selected points with valid work");
  if (!result || result->size() != 4) return;
  const auto& first = (*result)[0];
  check(first.reference_rate && first.reference_rate->operations_per_second == 1e8
    && first.reference_rate->units_per_second == 2e9, "rates scale by exact work identity");
  check(first.reference_rate && first.reference_rate->units_per_second_interval
    && std::abs(first.reference_rate->units_per_second_interval->low - 20e9 / 12) < 1e-5
    && first.reference_rate->units_per_second_interval->high == 2.5e9, "interval reverses endpoints under inversion");
  check((*result)[1].status == SweepStatus::skipped && !(*result)[1].work, "skipped point keeps absent work");
  check((*result)[2].status == SweepStatus::failed && (*result)[2].diagnostic == "fixture failure", "failure stays with point");
  check((*result)[3].status == SweepStatus::invalid_work && !(*result)[3].result, "zero work is not measured");

  const SweepPoint duplicate[] = {{Parameter::size, 10, "a"}, {Parameter::size, 10, "different label"}};
  const auto repeated = sweep(duplicate, select, run);
  check(!repeated && repeated.error() == SweepError::repeated_point && runs == 2,
    "duplicate identities fail before invoking either callback");
}

void scaling() {
  const ScalingPoint quadratic[] = {
    {Parameter::size, 1, 3}, {Parameter::size, 2, 12},
    {Parameter::size, 4, 48}, {Parameter::size, 8, 192},
  };
  const auto fit = analyze_scaling(quadratic);
  check(fit.status == ScalingStatus::observed && fit.observed == Trend::quadratic
    && fit.fits.size() == 6 && std::abs(fit.fits.front().multiplier - 3) < 1e-12,
    "log-space scaling distinguishes observed shapes and retains alternatives");
  const ScalingPoint short_range[] = {
    {Parameter::count, 101, 10}, {Parameter::count, 105, 10},
    {Parameter::count, 150, 10}, {Parameter::count, 201, 10},
  };
  check(analyze_scaling(short_range).status == ScalingStatus::short_range, "twofold range uses exact integers");
}

void recording() {
  Recorder recorder("request", 3, Retention::latest);
  recorder.record(100ns, Outcome::success);
  recorder.record(150ns, Outcome::failure, 2);
  {
    auto span = recorder.begin();
    check(recorder.snapshot().active == 1, "in-flight span is counted");
    auto moved = std::move(span);
    moved.finish(Outcome::timeout);
    moved.finish(Outcome::failure);
  }
  {
    [[maybe_unused]] auto abandoned = recorder.begin();
  }
  ProcessSample counters{};
  counters.cpu_time = 61ns;
  counters.peak_rss_bytes = 2048;
  recorder.record(70ns, Outcome::success, 1, counters);
  const auto snapshot = recorder.snapshot();
  check(snapshot.identity == "request" && snapshot.observed == 5 && snapshot.active == 0
    && snapshot.omitted() == 2 && snapshot.retained.size() == 3, "bounded latest recording with accurate counters");
  check(snapshot.counts[std::size_t(Outcome::success)] == 2 && snapshot.counts[std::size_t(Outcome::failure)] == 1
    && snapshot.counts[std::size_t(Outcome::timeout)] == 1
    && snapshot.counts[std::size_t(Outcome::abandoned)] == 1, "explicit outcomes and abandonment counted once");
  check(snapshot.retained.size() == 3 && snapshot.retained[0].sequence == 3
    && snapshot.retained[1].sequence == 4 && snapshot.retained[2].sequence == 5, "latest rows ordered by observation");
  const auto successes = snapshot.samples(Outcome::success);
  check(successes.size() == 1 && successes[0].elapsed == 70ns && successes[0].process
    && successes[0].process->peak_rss_bytes == 2048, "retained rows align timing, outcome and external metrics");

  Recorder a("a", 5, Retention::reservoir, 9);
  Recorder b("b", 5, Retention::reservoir, 9);
  for (int i = 1; i <= 50; ++i) {
    a.record(Nanoseconds(i), Outcome::success);
    b.record(Nanoseconds(i), Outcome::cancelled);
  }
  const auto first = a.snapshot();
  const auto second = b.snapshot();
  check(first.retained.size() == 5 && first.omitted() == 45, "reservoir stays within capacity");
  for (std::size_t i = 0; i < first.retained.size(); ++i)
    check(first.retained[i].sequence == second.retained[i].sequence, "reservoir keeps fixed-seed selections");

  Recorder shared("parallel requests", 12, Retention::first);
  {
    std::jthread one([&] {
      for (int i = 0; i < 64; ++i) shared.begin().finish(Outcome::success);
    });
    std::jthread two([&] {
      for (int i = 0; i < 64; ++i) shared.begin().finish(Outcome::cancelled);
    });
  }
  const auto parallel = shared.snapshot();
  check(parallel.active == 0 && parallel.observed == 128 && parallel.retained.size() == 12
    && parallel.counts[std::size_t(Outcome::success)] == 64
    && parallel.counts[std::size_t(Outcome::cancelled)] == 64, "concurrent spans retain total outcomes");
}

void independent() {
  std::vector<Sample> reference(15, sample(100ns));
  std::vector<Sample> contender(23, sample(50ns));
  Configuration config;
  config.bootstrap_resamples = 500;
  const auto speed = compare_independent(reference, contender, config);
  check(speed.speedup_interval && std::abs(speed.speedup - 2) < 1e-12
    && std::abs(speed.speedup_interval->low - 2) < 1e-12
    && std::abs(speed.speedup_interval->high - 2) < 1e-12
    && speed.verdict == Verdict::contender_faster, "independent resampling accepts unequal collection sizes");

  for (std::size_t i = 0; i < reference.size(); ++i) reference[i] = sample(Nanoseconds(10 * (i + 1)));
  const auto independent = compare_independent(reference, reference, config);
  const auto paired = summarize_paired(reference, reference, config);
  check(independent.speedup_interval && independent.speedup_interval->low < 0.99
    && independent.speedup_interval->high > 1.01 && independent.verdict == Verdict::inconclusive
    && paired.verdict == Verdict::equivalent, "independent resampling never invents round pairing");
}

}  // namespace

int main() {
  sweeps();
  scaling();
  recording();
  independent();
  return ycxx::unit::result("bench experiments");
}
