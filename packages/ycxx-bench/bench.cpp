// Report layout and statistical methods adapted from Flyology Bench. See LICENSE-FLYOLOGY.
module;
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
module ycxx.bench;
import std;

namespace ycxx::bench {
namespace detail {

std::pair<std::string_view, double> native_clock();
std::uint64_t rss_bytes(long amount);
std::optional<std::uint64_t> resident_memory();

std::optional<Nanoseconds> thread_cpu_now() {
  timespec time{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return {};
  return std::chrono::seconds(time.tv_sec) + Nanoseconds(time.tv_nsec);
}

ClockInfo clock_info() {
  const auto [source, nominal] = native_clock();
  std::array<Nanoseconds, 512> differences;
  for (auto& difference : differences) {
    const auto before = now_ns();
    const auto after = now_ns();
    difference = after - before;
  }
  std::ranges::sort(differences);
  const auto positive = std::ranges::upper_bound(differences, Nanoseconds{});
  return {source, nominal, positive == differences.end() ? Nanoseconds{} : *positive,
          differences[differences.size() / 2], differences.front()};
}

HostInfo host_info() {
  utsname info{};
  if (::uname(&info) != 0) return {};
  const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
  return {info.sysname, info.release, info.machine, cpus > 0 ? unsigned(cpus) : 0};
}

std::optional<ProcessCounters> process_info(bool starting) {
  std::optional<std::uint64_t> resident;
  std::optional<HostTicks> host;
  if (starting) {
    resident = resident_memory();
    host = host_cpu_ticks();
  }
  const auto thread_cpu = thread_cpu_now();
  rusage stats{};
  if (::getrusage(RUSAGE_SELF, &stats) != 0) return {};
  if (!starting) {
    host = host_cpu_ticks();
    resident = resident_memory();
  }
  const auto time = [](const auto& value) {
    return std::chrono::seconds(value.tv_sec) + std::chrono::microseconds(value.tv_usec);
  };
  const auto count = [](auto value) { return std::uint64_t(std::max(value, decltype(value){0})); };
  return ProcessCounters{
    time(stats.ru_utime) + time(stats.ru_stime), rss_bytes(stats.ru_maxrss), thread_cpu, resident,
    count(stats.ru_minflt), count(stats.ru_majflt), count(stats.ru_nvcsw), count(stats.ru_nivcsw),
    count(stats.ru_inblock), count(stats.ru_oublock), host};
}

ProcessSample process_sample(const ProcessCounters& before, const ProcessCounters& after) {
  const auto delta = [](auto start, auto end) { return end >= start ? end - start : decltype(end){}; };
  std::optional<Nanoseconds> thread;
  if (before.thread_cpu_time && after.thread_cpu_time)
    thread = delta(*before.thread_cpu_time, *after.thread_cpu_time);
  std::optional<double> busy;
  if (before.host && after.host && after.host->busy >= before.host->busy &&
      after.host->total > before.host->total) {
    const auto total = after.host->total - before.host->total;
    busy = double(std::min(after.host->busy - before.host->busy, total)) / double(total);
  }
  return {
    delta(before.cpu_time, after.cpu_time), after.peak_rss_bytes, thread, before.rss_bytes, after.rss_bytes,
    delta(before.minor_faults, after.minor_faults), delta(before.major_faults, after.major_faults),
    delta(before.voluntary_switches, after.voluntary_switches),
    delta(before.involuntary_switches, after.involuntary_switches),
    delta(before.block_input_operations, after.block_input_operations),
    delta(before.block_output_operations, after.block_output_operations), busy};
}

void validate(const Configuration& config) {
  contract_assert(config.warmup_time.count() >= 0);
  contract_assert(config.measurement_time.count() > 0);
  contract_assert(config.minimum_sample_time.count() > 0);
  contract_assert(config.maximum_sampling_time.count() >= 0);
  contract_assert(config.samples > 0
    && config.samples <= static_cast<std::size_t>(std::numeric_limits<Nanoseconds::rep>::max()));
  contract_assert(config.maximum_iterations > 0);
  contract_assert(config.practical_threshold >= 0 && config.practical_threshold < 1);
  contract_assert(config.confidence > 0 && config.confidence < 1);
}

}  // namespace detail

namespace {

double percentile(std::span<const double> ordered, double fraction) {
  const double rank = (ordered.size() - 1) * fraction;
  const auto lower = std::size_t(rank);
  const auto upper = std::min(lower + 1, ordered.size() - 1);
  return ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower);
}

double lag_one(std::span<const double> values) {
  if (values.size() < 3) return 0;
  double left = 0;
  double right = 0;
  for (std::size_t i = 1; i < values.size(); ++i) {
    left += values[i - 1];
    right += values[i];
  }
  left /= values.size() - 1;
  right /= values.size() - 1;
  double xy = 0;
  double xx = 0;
  double yy = 0;
  for (std::size_t i = 1; i < values.size(); ++i) {
    const auto x = values[i - 1] - left;
    const auto y = values[i] - right;
    xy += x * y;
    xx += x * x;
    yy += y * y;
  }
  return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
}

std::uint64_t next_random(std::uint64_t& state) {
  auto value = (state += 0x9e3779b97f4a7c15ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

// Circular blocks retain short-range sample order. Work with paired log-ratios
// for speedups, then exponentiate their mean and interval endpoints.
std::optional<Interval> bootstrap(std::span<const double> values, const Configuration& config) {
  if (values.size() < 10 || config.bootstrap_resamples == 0) return {};
  auto state = std::uint64_t{0xd1b54a32d192ed03ULL} ^ config.seed;
  const auto length = std::size_t(std::ceil(std::sqrt(double(values.size()))));
  std::vector<double> means;
  means.reserve(config.bootstrap_resamples);
  for (std::size_t draw = 0; draw != config.bootstrap_resamples; ++draw) {
    double sum = 0;
    for (std::size_t count = 0; count < values.size();) {
      const auto start = next_random(state) % values.size();
      for (std::size_t j = 0; j != length && count != values.size(); ++j, ++count)
        sum += values[(start + j) % values.size()];
    }
    means.push_back(sum / values.size());
  }
  std::ranges::sort(means);
  const double tail = (1 - config.confidence) / 2;
  return Interval{percentile(means, tail), percentile(means, 1 - tail)};
}

std::string_view verdict_name(Verdict verdict) {
  switch (verdict) {
    case Verdict::unavailable: return "unavailable";
    case Verdict::contender_faster: return "contender faster";
    case Verdict::reference_faster: return "reference faster";
    case Verdict::equivalent: return "practically equivalent";
    case Verdict::inconclusive: return "inconclusive";
  }
  std::unreachable();
}

std::string elapsed(double ns) {
  if (ns >= 1e9) return std::format("{:.3f} s", ns / 1e9);
  if (ns >= 1e6) return std::format("{:.3f} ms", ns / 1e6);
  if (ns >= 1e3) return std::format("{:.3f} µs", ns / 1e3);
  if (ns > 0 && ns < 1) return std::format("{:.3f} ps", ns * 1e3);
  return std::format("{:.3f} ns", ns);
}

std::string sparkline(std::span<const double> values, std::size_t limit) {
  constexpr std::string_view bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  if (values.empty()) return {};
  auto [lo, hi] = std::ranges::minmax_element(values);
  const auto width = std::min(values.size(), limit);
  std::string line;
  for (std::size_t i = 0; i != width; ++i) {
    const auto from = i * values.size() / width;
    const auto to = (i + 1) * values.size() / width;
    double sum = 0;
    for (auto j = from; j != to; ++j) sum += values[j];
    const double sample = sum / (to - from);
    const auto index = *hi > *lo ? std::size_t((sample - *lo) / (*hi - *lo) * 7 + 0.5) : 0;
    line += bars[std::min(index, std::size_t{7})];
  }
  return line;
}

std::string_view styled(Style style, std::string_view escape_code) {
  return style == Style::ansi ? escape_code : std::string_view{};
}

std::size_t terminal_width(std::ostream& out) {
  int fd = &out == &std::cout ? STDOUT_FILENO : &out == &std::cerr ? STDERR_FILENO : -1;
  winsize size{};
  if (fd >= 0 && ::ioctl(fd, TIOCGWINSZ, &size) == 0) return size.ws_col;
  return 0;
}

std::size_t report_width(std::ostream& out) {
  const auto width = terminal_width(out);
  return width == 0 ? 112 : std::clamp(width, std::size_t{36}, std::size_t{112});
}

std::string rule(std::size_t length) {
  std::string line;
  for (std::size_t i = 0; i != length; ++i) line += "─";
  return line;
}

// Most report text is ASCII. Keep UTF-8 code points whole on wrapped rows.
std::size_t text_width(std::string_view text) {
  return std::ranges::count_if(text, [](unsigned char ch) { return (ch & 0xc0) != 0x80; });
}

void title(std::ostream& out, std::string_view label, Style style) {
  const auto width = report_width(out);
  const auto used = std::min(text_width(label) + 7, width);
  std::println(out, "{}━┥ {} ┝{}{}", styled(style, "\033[1;36m"), label,
    rule(width - used), styled(style, "\033[0m"));
}

void row(std::ostream& out, std::string_view label, std::string_view value,
         Style style, std::string_view color = "\033[2m") {
  constexpr std::size_t prefix = 16;
  const auto limit = report_width(out) - prefix;
  bool first = true;
  while (true) {
    std::size_t length = 0;
    std::size_t next = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
      const auto ch = static_cast<unsigned char>(value[i]);
      if ((ch & 0xc0) == 0x80) continue;
      if (ch == ' ' && i > 0) next = i;
      if (++length > limit) {
        if (next == 0) next = i;
        break;
      }
    }
    const auto end = length > limit ? next : value.size();
    std::println(out, "   {}{:<10}{} │ {}", styled(style, color), first ? label : "",
      styled(style, "\033[0m"), value.substr(0, end));
    if (end == value.size()) break;
    value.remove_prefix(end + (value[end] == ' ' ? 1 : 0));
    first = false;
  }
}

std::string time_change(double relative) {
  if (std::abs(relative) < 0.00005) return "same";
  return std::format("{:.2f}% {}", std::abs(relative) * 100, relative < 0 ? "less" : "more");
}

std::string_view verdict_color(Verdict verdict) {
  switch (verdict) {
    case Verdict::contender_faster: return "\033[1;32m";
    case Verdict::reference_faster: return "\033[1;33m";
    case Verdict::equivalent: return "\033[1;36m";
    case Verdict::inconclusive:
    case Verdict::unavailable: return "\033[2m";
  }
  std::unreachable();
}

void comparison_row(std::ostream& out, std::string_view name, const Measurement& result,
                    const PairedSummary& paired, Style style) {
  const auto ci = paired.speedup_interval
    ? std::format("[{:.3f}, {:.3f}]", paired.speedup_interval->low, paired.speedup_interval->high)
    : std::string("—");
  const auto tint = verdict_color(paired.verdict);
  const auto speed = paired.speedup > 0 ? std::format("{:.3f}x", paired.speedup) : std::string("—");
  const auto change = paired.speedup > 0 ? time_change(paired.relative_time_change) : std::string("—");
  if (report_width(out) < 104 || text_width(name) > 21) {
    std::println(out, "   {}{}{}", styled(style, tint), name, styled(style, "\033[0m"));
    row(out, "latency", std::format("{}/op  ·  {} speedup  ·  {} elapsed", elapsed(result.summary.median),
      speed, change), style);
    row(out, "decision", std::format("{}  ·  {} speedup interval", verdict_name(paired.verdict), ci), style, tint);
    return;
  }
  std::println(out, "   {:<21} {:>12} {:>9} {:>13} {:>17}  {}{}{}",
    name, elapsed(result.summary.median), speed, change, ci,
    styled(style, tint), verdict_name(paired.verdict), styled(style, "\033[0m"));
}

void comparison_headers(std::ostream& out, std::string_view reference, const Measurement& result, Style style) {
  if (report_width(out) < 104 || text_width(reference) > 21) {
    row(out, "reference", std::format("{}  ·  median {}/op", reference, elapsed(result.summary.median)), style);
    return;
  }
  std::println(out, "{}   {:<21} {:>12} {:>9} {:>13} {:>17}  {}{}",
    styled(style, "\033[2m"), "implementation", "median", "speedup", "elapsed time", "speedup interval",
    "decision", styled(style, "\033[0m"));
  std::println(out, "   {:<21} {:>12} {:>9} {:>13} {:>17}  {}",
    reference, elapsed(result.summary.median), "1.000x", "reference", "—", "reference");
}

void effect_plot(std::ostream& out, std::span<const std::string_view> names,
                 std::span<const PairedSummary> pairs, double threshold, Style style) {
  double extent = 0;
  for (const auto& pair : pairs) {
    if (pair.speedup > 0) extent = std::max(extent, std::abs(std::log(pair.speedup)));
    if (pair.speedup_interval) {
      extent = std::max(extent, std::abs(std::log(pair.speedup_interval->low)));
      extent = std::max(extent, std::abs(std::log(pair.speedup_interval->high)));
    }
  }
  if (extent == 0 && std::ranges::none_of(pairs, [](const auto& pair) { return pair.speedup > 0; })) return;
  extent = std::max(1.08 * extent, -1.1 * std::log1p(-threshold) + 0.001);
  const auto columns = report_width(out);
  const bool compact = columns < 78
    || std::ranges::any_of(names, [&](auto name) { return text_width(name) > columns - 51; });
  const std::size_t graph_width = compact ? std::min(columns - 20, std::size_t{39}) : 47;
  const auto point = [&](double log_elapsed) {
    const auto normalized = std::clamp((log_elapsed / extent + 1) / 2, 0.0, 1.0);
    return std::size_t(std::round(normalized * (graph_width - 1)));
  };
  const auto zero = point(0);
  const auto threshold_low = point(std::log1p(-threshold));
  const auto threshold_high = point(std::log1p(threshold));
  const auto axis = [&](std::string_view plot, std::string_view name, std::string_view tint) {
    if (!compact) std::print(out, "   {:<23} ", name);
    else if (!name.empty()) std::println(out, "   {}", name);
    if (compact) std::print(out, "   ");
    std::println(out, "{}{}{}", styled(style, tint), plot, styled(style, "\033[0m"));
  };
  std::println(out);
  row(out, "effect", compact ? "Elapsed vs reference (log scale)"
    : "Elapsed vs reference (log scale)  ·  ◆ estimate  ━ interval  ┊ equal", style, "\033[1;36m");
  const auto low_label = std::format("{:+.0f}%", std::expm1(-extent) * 100);
  const auto high_label = std::format("{:+.0f}%", std::expm1(extent) * 100);
  if (low_label.size() + high_label.size() + 2 <= graph_width) {
    const auto tick = std::clamp(zero, low_label.size() + 1, graph_width - high_label.size() - 1);
    axis(std::format("{}{}0{}{}", low_label, std::string(tick - low_label.size(), ' '),
      std::string(graph_width - tick - 1 - high_label.size(), ' '), high_label), "", "\033[2m");
  }
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const auto& pair = pairs[i];
    if (pair.speedup <= 0) continue;
    const auto estimate = point(-std::log(pair.speedup));
    const auto low = pair.speedup_interval ? point(-std::log(pair.speedup_interval->high)) : estimate;
    const auto high = pair.speedup_interval ? point(-std::log(pair.speedup_interval->low)) : estimate;
    std::string plot;
    for (std::size_t x = 0; x < graph_width; ++x) {
      if (x == estimate) plot += "◆";
      else if (pair.speedup_interval && x == low) plot += "├";
      else if (pair.speedup_interval && x == high) plot += "┤";
      else if (x > low && x < high) plot += "━";
      else if (x == zero) plot += "┊";
      else if (x >= threshold_low && x <= threshold_high) plot += "·";
      else plot += " ";
    }
    axis(plot, names[i], verdict_color(pair.verdict));
  }
  if (compact) row(out, "legend", "◆ estimate  ━ interval  ┊ equal", style);
}

std::string quote_json(std::string_view value) {
  std::string result = "\"";
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (ch == '\\' || ch == '"') {
      result += '\\';
      result += ch;
    } else if (byte < 32) result += std::format("\\u{:04x}", unsigned(byte));
    else result += ch;
  }
  result += '"';
  return result;
}

std::string quote_csv(std::string_view value) {
  std::string result = "\"";
  for (const auto ch : value) {
    if (ch == '"') result += '"';
    result += ch;
  }
  result += '"';
  return result;
}

std::string format_metadata(const MetadataValue& value, bool json) {
  return std::visit([&](const auto& entry) -> std::string {
    using T = std::remove_cvref_t<decltype(entry)>;
    if constexpr (std::same_as<T, std::string>) return json ? quote_json(entry) : entry;
    else if constexpr (std::same_as<T, double>) {
      if (json && !std::isfinite(entry)) return "null";
      return std::format("{}", entry);
    } else return std::format("{}", entry);
  }, value);
}

std::string metadata_json(const Metadata& metadata) {
  std::string result = "[";
  for (const auto& entry : metadata) {
    if (result.size() > 1) result += ',';
    result += std::format("{{\"name\":{},\"value\":{}}}",
      quote_json(entry.name), format_metadata(entry.value, true));
  }
  result += ']';
  return result;
}

std::string metadata_console(const Metadata& metadata) {
  std::string result;
  for (const auto& entry : metadata) {
    if (!result.empty()) result += "  ·  ";
    result += std::format("{} {}", entry.name, format_metadata(entry.value, false));
  }
  return result;
}

std::string properties_text(const Properties& value, bool json) {
  std::string tags;
  for (const auto& tag : value.tags) {
    if (!tags.empty()) tags += json ? "," : ", ";
    tags += json ? quote_json(tag) : tag;
  }
  if (json) return std::format("{{\"group\":{},\"tags\":[{}],\"band\":{}}}",
    quote_json(value.group), tags, quote_json(band_name(value.band)));
  std::string result;
  if (!value.group.empty()) result = "group " + value.group;
  if (!tags.empty()) {
    if (!result.empty()) result += "  ·  ";
    result += "tags " + tags;
  }
  if (value.band != Band::inherit) {
    if (!result.empty()) result += "  ·  ";
    result += std::format("band {}", band_name(value.band));
  }
  return result;
}

std::string json_interval(std::optional<Interval> value) {
  if (!value) return "null";
  return std::format("[{},{}]", value->low, value->high);
}

template<class T>
std::string optional_number(std::optional<T> value, std::string_view unavailable = "null") {
  return value ? std::format("{}", *value) : std::string(unavailable);
}

std::optional<Nanoseconds::rep> cpu_count(const ProcessSample& data) {
  if (data.thread_cpu_time) return data.thread_cpu_time->count();
  return {};
}

void sample_json(std::ostream& out, const Sample& sample, Nanoseconds timer_cost = {}) {
  std::print(out, "{{\"iterations\":{},\"elapsed_ns\":{},\"ns_per_operation\":{}",
    sample.iterations, sample.elapsed.count(), sample.ns_per_operation(timer_cost));
  if (sample.process) {
    const auto& process = *sample.process;
    std::print(out, ",\"process_cpu_ns\":{},\"peak_rss_bytes\":{},\"thread_cpu_ns\":{},"
      "\"rss_before_bytes\":{},\"rss_after_bytes\":{},\"minor_faults\":{},\"major_faults\":{},"
      "\"voluntary_switches\":{},\"involuntary_switches\":{},\"block_input_operations\":{},"
      "\"block_output_operations\":{},\"host_cpu_busy_fraction\":{}",
      process.cpu_time.count(), process.peak_rss_bytes, optional_number(cpu_count(process)),
      optional_number(process.rss_before_bytes), optional_number(process.rss_after_bytes),
      process.minor_faults, process.major_faults, process.voluntary_switches, process.involuntary_switches,
      process.block_input_operations, process.block_output_operations,
      optional_number(process.host_cpu_busy_fraction));
  }
  std::print(out, "}}");
}

void measurement_json(std::ostream& out, const Measurement& result) {
  const auto& s = result.summary;
  const auto& policy = result.policy;
  std::print(out, "{{\"properties\":{},\"metadata\":{},\"sampling_policy\":{{\"effective_band\":{},"
    "\"warmup_ns\":{},\"target_batch_ns\":{},\"collection_limit_ns\":{},"
    "\"requested_samples\":{},\"bootstrap_resamples\":{},\"seed\":{},\"batching\":{}}},",
    properties_text(result.properties, true), metadata_json(result.metadata),
    quote_json(policy.band == Band::inherit ? "explicit" : band_name(policy.band)),
    policy.warmup.count(), policy.target_batch.count(), policy.collection_limit.count(),
    policy.requested_samples, policy.bootstrap_resamples, policy.seed,
    quote_json(policy.batching == Batching::equal_time ? "equal_time" : "shared_iterations"));
  std::print(out, "\"clock\":{{\"source\":{},\"nominal_ns\":{},\"observed_ns\":{},"
    "\"median_read_ns\":{},\"minimum_read_ns\":{},\"floor_ns_per_operation\":{}}},\"host\":{{"
    "\"system\":{},\"release\":{},\"machine\":{},\"logical_cpus\":{}}},\"confidence\":{},"
    "\"sampling_limited\":{},\"timer_cost_ns\":{},\"summary_ns_per_operation\":{{\"minimum\":{},"
    "\"median\":{},\"mean\":{},\"maximum\":{},\"p95\":{},\"p99\":{},\"mad\":{},\"mean_interval\":{}}},"
    "\"quality\":{{\"coefficient_of_variation\":{},\"lag_one\":{},\"mild_outliers\":{},"
    "\"severe_outliers\":{}}},\"samples\":[",
    quote_json(result.clock.source), result.clock.nominal_ns, result.clock.observed.count(),
    result.clock.median_read.count(), result.clock.minimum_read.count(), result.clock_floor_ns_per_operation(),
    quote_json(result.host.system), quote_json(result.host.release), quote_json(result.host.machine),
    result.host.logical_cpus, result.confidence, result.sampling_limited, result.timer_cost.count(),
    s.minimum, s.median, s.mean, s.maximum, s.p95, s.p99, s.mad, json_interval(s.mean_interval),
    s.coefficient_of_variation, s.lag_one, s.mild_outliers, s.severe_outliers);
  for (std::size_t i = 0; i != result.samples.size(); ++i) {
    if (i != 0) std::print(out, ",");
    sample_json(out, result.samples[i], result.timer_cost);
  }
  std::print(out, "]}}");
}

}  // namespace

Configuration configuration_for(Band band) {
  using namespace std::chrono_literals;
  Configuration config;
  config.profile = band == Band::inherit ? Band::standard : band;
  if (band == Band::smoke) {
    config.warmup_time = 15ms;
    config.measurement_time = 60ms;
    config.samples = 15;
    config.bootstrap_resamples = 500;
  } else if (band == Band::deep) {
    config.warmup_time = 500ms;
    config.measurement_time = 3s;
    config.samples = 120;
    config.bootstrap_resamples = 5'000;
  }
  return config;
}

std::string_view band_name(Band band) {
  switch (band) {
    case Band::inherit: return "inherit";
    case Band::smoke: return "smoke";
    case Band::standard: return "standard";
    case Band::deep: return "deep";
  }
  std::unreachable();
}

bool matches(const Properties& properties, const Selection& selection) {
  if (!selection.group.empty() && selection.group != properties.group) return false;
  if (selection.band && *selection.band != properties.band) return false;
  return std::ranges::all_of(selection.tags, [&](auto tag) {
    return std::ranges::find(properties.tags, tag) != properties.tags.end();
  });
}

bool stdout_is_terminal() { return ::isatty(STDOUT_FILENO) != 0; }

double Measurement::clock_floor_ns_per_operation() const {
  if (samples.empty()) return 0;
  const auto count = std::ranges::min_element(samples, {}, &Sample::iterations)->iterations;
  if (count == 0) return 0;
  return std::max(clock.nominal_ns, double(clock.observed.count())) / count;
}

TerminalProgress::TerminalProgress(std::ostream& out, std::size_t cases, Band profile)
  : out_(out), cases_(cases), profile_(profile) {}

TerminalProgress::~TerminalProgress() {
  try {
    clear();
  } catch (...) {
    // Clearing is best effort if the caller's stream has already failed.
  }
}

void TerminalProgress::operator()(Progress progress) {
  using namespace std::chrono_literals;
  const auto columns = terminal_width(out_);
  if (columns > 0 && columns < 36) return;
  const auto now = Clock::now();
  if (phase_ == progress.phase && progress.completed != progress.total && now - last_ < 45ms) return;
  if (phase_ == progress.phase && progress.completed == completed_ && progress.total == total_) return;
  phase_ = progress.phase;
  completed_ = progress.completed;
  total_ = progress.total;
  last_ = now;
  clear();

  const auto seconds = std::chrono::duration<double>(now - started_).count();
  constexpr std::string_view short_names[] = {"warm", "cal", "sample", "stats"};
  constexpr std::string_view full_names[] = {"warmup", "calibrate", "sample", "analyze"};
  const auto* names = columns != 0 && columns < 53 ? short_names : full_names;
  if (names == short_names)
    std::println(out_, "   \033[1;36mycxx bench\033[0m  {} {} · {:.2f}s", cases_, cases_ == 1 ? "case" : "cases", seconds);
  else
    std::println(out_, "   \033[1;36mycxx bench\033[0m  {} {} · {} · {:.2f}s",
      cases_, cases_ == 1 ? "case" : "cases", band_name(profile_), seconds);

  const auto active = std::size_t(progress.phase);
  std::print(out_, "   ");
  for (std::size_t i = 0; i < std::size(full_names); ++i) {
    if (i != 0) std::print(out_, "  ");
    if (i < active) std::print(out_, "\033[32m✓ {}\033[0m", names[i]);
    else if (i == active) std::print(out_, "\033[1;36m● {}\033[0m", names[i]);
    else std::print(out_, "\033[2m· {}\033[0m", names[i]);
  }
  std::println(out_);

  if (progress.total != 0) {
    const std::size_t width = names == short_names ? 16 : 24;
    const auto fraction = std::clamp(double(progress.completed) / progress.total, 0.0, 1.0);
    const auto filled = std::size_t(width * fraction);
    std::string bar;
    for (std::size_t i = 0; i != width; ++i) bar += i < filled ? "━" : "─";
    std::print(out_, "   \033[36m{}\033[0m  {}/{}{}", bar, progress.completed, progress.total,
      progress.phase == Phase::warming ? "%" : " rounds");
  } else {
    std::print(out_, "   \033[2m{}\033[0m", progress.phase == Phase::calibrating
      ? "choosing operations per batch" : "summarizing measured batches");
  }
  out_.flush();
  visible_ = true;
}

void TerminalProgress::clear() {
  if (!visible_) return;
  std::print(out_, "\r\033[2K\033[1A\033[2K\033[1A\033[2K");
  out_.flush();
  visible_ = false;
}

Summary summarize(std::span<const Sample> samples, const Configuration& config, Nanoseconds timer_cost) {
  detail::validate(config);
  Summary result;
  if (samples.empty()) return result;
  std::vector<double> values;
  values.reserve(samples.size());
  double squared_deviation = 0;
  for (const auto& sample : samples) {
    const double value = sample.ns_per_operation(timer_cost);
    values.push_back(value);
    const double difference = value - result.mean;
    result.mean += difference / values.size();
    squared_deviation += difference * (value - result.mean);
  }
  if (values.size() > 1 && result.mean > 0)
    result.coefficient_of_variation = std::sqrt(std::max(0.0, squared_deviation) / (values.size() - 1)) / result.mean;
  result.lag_one = lag_one(values);
  result.mean_interval = bootstrap(values, config);
  std::ranges::sort(values);
  result.minimum = values.front();
  result.maximum = values.back();
  result.median = percentile(values, 0.5);
  result.p95 = percentile(values, 0.95);
  result.p99 = percentile(values, 0.99);
  const double q1 = percentile(values, 0.25);
  const double q3 = percentile(values, 0.75);
  const double iqr = q3 - q1;
  for (auto& value : values) {
    const double outside = std::max(q1 - value, value - q3);
    if (outside > 3 * iqr) ++result.severe_outliers;
    else if (outside > 1.5 * iqr) ++result.mild_outliers;
    value = std::abs(value - result.median);
  }
  std::ranges::sort(values);
  result.mad = percentile(values, 0.5);
  return result;
}

PairedSummary summarize_paired(std::span<const Sample> reference, std::span<const Sample> contender,
                               const Configuration& config, Nanoseconds timer_cost) {
  detail::validate(config);
  PairedSummary result;
  if (reference.empty() || reference.size() != contender.size()) return result;
  std::vector<double> ratios;
  ratios.reserve(reference.size());
  double sum = 0;
  for (std::size_t i = 0; i != reference.size(); ++i) {
    const double a = reference[i].ns_per_operation(timer_cost);
    const double b = contender[i].ns_per_operation(timer_cost);
    if (a <= 0 || b <= 0) return result;
    const auto ratio = std::log(a / b);
    ratios.push_back(ratio);
    sum += ratio;
  }
  result.speedup = std::exp(sum / ratios.size());
  result.relative_time_change = std::exp(-sum / ratios.size()) - 1;
  result.lag_one = lag_one(ratios);
  if (const auto limits = bootstrap(ratios, config)) {
    result.speedup_interval = Interval{std::exp(limits->low), std::exp(limits->high)};
    const double fastest = std::exp(-limits->high) - 1;
    const double slowest = std::exp(-limits->low) - 1;
    const double threshold = config.practical_threshold;
    if (slowest < -threshold) result.verdict = Verdict::contender_faster;
    else if (fastest > threshold) result.verdict = Verdict::reference_faster;
    else if (fastest >= -threshold && slowest <= threshold) result.verdict = Verdict::equivalent;
    else result.verdict = Verdict::inconclusive;
  }
  return result;
}

void write_console(std::ostream& out, std::string_view name, const Measurement& data, Style style) {
  title(out, name, style);
  if (data.samples.empty()) {
    row(out, "sampling", "no samples", style);
    return;
  }
  if (const auto properties = properties_text(data.properties, false); !properties.empty())
    row(out, "case", properties, style);
  if (!data.metadata.empty()) row(out, "workload", metadata_console(data.metadata), style);
  const auto& s = data.summary;
  const auto iterations = data.samples.front().iterations;
  std::vector<double> raw;
  std::vector<double> process_cpu;
  std::vector<double> process_rss;
  std::vector<double> thread_cpu;
  std::vector<double> host_cpu;
  std::uint64_t peak_rss = 0;
  std::uint64_t minor_faults = 0;
  std::uint64_t major_faults = 0;
  std::uint64_t switches = 0;
  std::uint64_t block_operations = 0;
  double total_ns = 0;
  for (const auto& sample : data.samples) {
    const double sample_ns = double(sample.elapsed.count());
    total_ns += sample_ns;
    raw.push_back(sample.ns_per_operation(data.timer_cost));
    if (sample.process && sample_ns > 0) {
      const auto& process = *sample.process;
      process_cpu.push_back(double(process.cpu_time.count()) / sample_ns * 100);
      if (process.rss_after_bytes) process_rss.push_back(double(*process.rss_after_bytes));
      if (process.thread_cpu_time) thread_cpu.push_back(double(process.thread_cpu_time->count()) / sample_ns * 100);
      if (process.host_cpu_busy_fraction) host_cpu.push_back(*process.host_cpu_busy_fraction * 100);
      peak_rss = std::max(peak_rss, process.peak_rss_bytes);
      minor_faults += process.minor_faults;
      major_faults += process.major_faults;
      switches += process.voluntary_switches + process.involuntary_switches;
      block_operations += process.block_input_operations + process.block_output_operations;
    }
  }
  const auto trace_width = std::min(report_width(out) - 18, std::size_t{56});
  std::println(out, "   {}{}/op{}  median   ·   {} mean",
    styled(style, "\033[1;36m"), elapsed(s.median), styled(style, "\033[0m"), elapsed(s.mean));
  row(out, "tails", std::format("p95 {}  p99 {}  range {} … {}", elapsed(s.p95), elapsed(s.p99),
    elapsed(s.minimum), elapsed(s.maximum)), style, "\033[36m");
  if (s.mean_interval)
    row(out, "confidence", std::format("{:g}% mean interval {} … {}", data.confidence * 100,
      elapsed(s.mean_interval->low), elapsed(s.mean_interval->high)), style);
  row(out, "sampling", std::format("{} iter/sample × {} samples  median batch {}{}",
    iterations, data.samples.size(), elapsed(s.median * iterations + data.timer_cost.count()),
    data.sampling_limited ? "  (collection limit)" : ""), style);
  row(out, "warmup", std::format("{} target  ·  {} target batch  ·  {} policy",
    elapsed(double(data.policy.warmup.count())), elapsed(double(data.policy.target_batch.count())),
    data.policy.band == Band::inherit ? "explicit" : band_name(data.policy.band)), style);
  row(out, "quality", std::format("CV {:.2f}%  MAD {}  outliers {}  lag-1 {:+.2f}",
    s.coefficient_of_variation * 100, elapsed(s.mad), s.mild_outliers + s.severe_outliers, s.lag_one),
    style, s.coefficient_of_variation > 0.10 || s.mild_outliers + s.severe_outliers ? "\033[33m" : "\033[32m");
  row(out, "batch trace", sparkline(raw, trace_width), style, "\033[36m");
  row(out, "clock", std::format("{}  nominal {}  observed {}  median read {}",
    data.clock.source, elapsed(data.clock.nominal_ns), elapsed(double(data.clock.observed.count())),
    elapsed(double(data.clock.median_read.count()))), style);
  row(out, "resolution", std::format("batch floor {}/op  timer reduction {}",
    elapsed(data.clock_floor_ns_per_operation()), elapsed(double(data.timer_cost.count()))), style);
  if (!process_cpu.empty()) {
    const auto mean = [](std::span<const double> values) {
      return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    const auto width = std::min(trace_width, std::size_t{24});
    auto cpu = std::format("{}  process {:.1f}%", sparkline(process_cpu, width), mean(process_cpu));
    if (!thread_cpu.empty()) cpu += std::format("  ·  thread {:.1f}%", mean(thread_cpu));
    row(out, "cpu", cpu, style, "\033[36m");
    auto memory = std::format("peak {:.2f} MiB", double(peak_rss) / (1024 * 1024));
    if (!process_rss.empty())
      memory = std::format("{}  resident {:.2f} MiB  ·  {}", sparkline(process_rss, width),
        process_rss.back() / (1024 * 1024), memory);
    row(out, "memory", memory, style);
    if (!host_cpu.empty())
      row(out, "host load", std::format("{:.1f}% busy  ·  {} observed batches", mean(host_cpu), host_cpu.size()), style);
    if (minor_faults || major_faults || switches || block_operations)
      row(out, "kernel", std::format("{} minor / {} major faults  ·  {} switches  ·  {} block I/O ops",
        minor_faults, major_faults, switches, block_operations), style);
  }
  row(out, "host", std::format("{} {} · {} · {} logical CPUs",
    data.host.system, data.host.release, data.host.machine, data.host.logical_cpus), style);
  row(out, "elapsed", std::format("{} timed samples", elapsed(total_ns)), style);
}

namespace {

void comparison_method(std::ostream& out, const Measurement& data, double threshold, Style style) {
  row(out, "method", std::format("{} rounds  ·  {:g}% interval  ·  {:g}% threshold  ·  {}",
    data.samples.size(), data.confidence * 100, threshold * 100,
    data.policy.band == Band::inherit ? "explicit" : band_name(data.policy.band)), style);
}

}  // namespace

void write_console(std::ostream& out, std::string_view reference, std::string_view contender,
                   const Comparison& data, Style style) {
  title(out, "paired comparison", style);
  comparison_headers(out, reference, data.reference, style);
  comparison_row(out, contender, data.contender, data.paired, style);
  effect_plot(out, std::span(&contender, 1), std::span(&data.paired, 1), data.practical_threshold, style);
  std::println(out);
  comparison_method(out, data.reference, data.practical_threshold, style);
}

void write_console(std::ostream& out, std::span<const std::string_view> names, const Shootout& data, Style style) {
  contract_assert(names.size() >= 2 && names.size() == data.cases.size() && names.size() == data.paired.size());
  title(out, "balanced shootout", style);
  comparison_headers(out, names.front(), data.cases.front(), style);
  for (std::size_t i = 1; i != names.size(); ++i) comparison_row(out, names[i], data.cases[i], data.paired[i], style);
  effect_plot(out, names.subspan(1), std::span(data.paired).subspan(1), data.practical_threshold, style);
  std::println(out);
  comparison_method(out, data.cases.front(), data.practical_threshold, style);
}

void write_ndjson(std::ostream& out, std::string_view name, const Measurement& data) {
  std::print(out, "{{\"schema\":1,\"event\":\"report\",\"case\":{},\"measurement\":", quote_json(name));
  measurement_json(out, data);
  std::println(out, "}}");
}

static void paired_json(std::ostream& out, const PairedSummary& pair) {
  std::print(out, "{{\"speedup\":{},\"relative_time_change\":{},\"lag_one\":{},"
    "\"speedup_interval\":{},\"verdict\":{}}}",
    pair.speedup, pair.relative_time_change, pair.lag_one,
    json_interval(pair.speedup_interval), quote_json(verdict_name(pair.verdict)));
}

void write_ndjson(std::ostream& out, std::string_view reference, std::string_view contender, const Comparison& data) {
  std::print(out, "{{\"schema\":1,\"event\":\"report\",\"reference_case\":{},\"contender_case\":{},"
    "\"contender_first_in_round_zero\":{},\"practical_threshold\":{},\"paired\":",
    quote_json(reference), quote_json(contender), data.contender_first_in_round_zero, data.practical_threshold);
  paired_json(out, data.paired);
  std::print(out, ",\"reference\":");
  measurement_json(out, data.reference);
  std::print(out, ",\"contender\":");
  measurement_json(out, data.contender);
  std::println(out, "}}");
}

void write_ndjson(std::ostream& out, std::span<const std::string_view> names, const Shootout& data) {
  contract_assert(names.size() >= 2 && names.size() == data.cases.size() && names.size() == data.paired.size());
  std::print(out, "{{\"schema\":1,\"event\":\"report\",\"first_case_in_round_zero\":{},\"practical_threshold\":{},\"cases\":[",
    data.first_case_in_round_zero, data.practical_threshold);
  for (std::size_t i = 0; i != names.size(); ++i) {
    if (i != 0) std::print(out, ",");
    std::print(out, "{{\"case\":{},\"measurement\":", quote_json(names[i]));
    measurement_json(out, data.cases[i]);
    std::print(out, ",\"paired_with_reference\":");
    if (i == 0) std::print(out, "null");
    else paired_json(out, data.paired[i]);
    std::print(out, "}}");
  }
  std::println(out, "]}}");
}

void write_live_start(std::ostream& out, std::span<const std::string_view> names) {
  std::print(out, "{{\"schema\":1,\"event\":\"start\",\"cases\":[");
  for (std::size_t i = 0; i != names.size(); ++i) {
    if (i != 0) std::print(out, ",");
    std::print(out, "{}", quote_json(names[i]));
  }
  std::println(out, "]}}");
  out.flush();
}

void write_live_progress(std::ostream& out, Progress progress) {
  constexpr std::string_view phase[] = {"warming", "calibrating", "sampling", "analyzing"};
  std::println(out, "{{\"schema\":1,\"event\":\"progress\",\"phase\":{},\"completed\":{},\"total\":{}}}",
    quote_json(phase[std::to_underlying(progress.phase)]), progress.completed, progress.total);
  out.flush();
}

void write_live_sample(std::ostream& out, std::string_view name, std::size_t round, const Sample& sample) {
  std::print(out, "{{\"schema\":1,\"event\":\"sample\",\"case\":{},\"round\":{},\"raw\":",
    quote_json(name), round + 1);
  sample_json(out, sample);
  std::println(out, "}}");
  out.flush();
}

void write_csv(std::ostream& out, std::string_view name, const Measurement& data) {
  std::println(out, "case,sample,iterations,elapsed_ns,ns_per_operation,process_cpu_ns,peak_rss_bytes,metadata_json,properties_json,"
    "thread_cpu_ns,rss_before_bytes,rss_after_bytes,minor_faults,major_faults,voluntary_switches,"
    "involuntary_switches,block_input_operations,block_output_operations,host_cpu_busy_fraction");
  const auto quoted = quote_csv(name);
  const auto metadata = quote_csv(metadata_json(data.metadata));
  const auto properties = quote_csv(properties_text(data.properties, true));
  for (std::size_t i = 0; i != data.samples.size(); ++i) {
    const auto& sample = data.samples[i];
    std::print(out, "{},{},{},{},{},", quoted, i + 1, sample.iterations,
      sample.elapsed.count(), sample.ns_per_operation(data.timer_cost));
    if (!sample.process) {
      std::println(out, ",,{},{}{}", metadata, properties, std::string(10, ','));
      continue;
    }
    const auto& process = *sample.process;
    std::println(out, "{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
      process.cpu_time.count(), process.peak_rss_bytes, metadata, properties,
      optional_number(cpu_count(process), ""), optional_number(process.rss_before_bytes, ""),
      optional_number(process.rss_after_bytes, ""), process.minor_faults, process.major_faults,
      process.voluntary_switches, process.involuntary_switches, process.block_input_operations,
      process.block_output_operations, optional_number(process.host_cpu_busy_fraction, ""));
  }
}

}  // namespace ycxx::bench
