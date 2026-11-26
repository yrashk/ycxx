module;
#include <time.h>
module ycxx.bench;
import std;

namespace ycxx::bench::detail {

Nanoseconds now_ns() {
  timespec time{};
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0)
    throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
  return std::chrono::seconds(time.tv_sec) + Nanoseconds(time.tv_nsec);
}

std::pair<std::string_view, double> native_clock() {
  timespec time{};
  if (::clock_getres(CLOCK_MONOTONIC_RAW, &time) != 0)
    throw std::runtime_error("clock_getres(CLOCK_MONOTONIC_RAW) failed");
  return {"CLOCK_MONOTONIC_RAW", double((std::chrono::seconds(time.tv_sec) + Nanoseconds(time.tv_nsec)).count())};
}

std::optional<std::uint64_t> rusage_max_rss_bytes(long amount) {
  if (amount <= 0 || std::uint64_t(amount) > std::numeric_limits<std::uint64_t>::max() / 1024) return {};
  return std::uint64_t(amount) * 1024;
}

std::optional<ResidentMemory> resident_memory() {
  std::ifstream status("/proc/self/status");
  std::optional<std::uint64_t> resident;
  std::optional<std::uint64_t> high_water;
  const auto bytes = [](std::string_view value) -> std::optional<std::uint64_t> {
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    value.remove_prefix(start);
    std::uint64_t kib{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), kib);
    if (parsed.ec != std::errc{} || kib > std::numeric_limits<std::uint64_t>::max() / 1024) return {};
    value.remove_prefix(parsed.ptr - value.data());
    const auto suffix = value.find_first_not_of(" \t");
    if (suffix == std::string_view::npos || value.substr(suffix) != "kB") return {};
    return kib * 1024;
  };

  std::string line;
  while (std::getline(status, line)) {
    const std::string_view text(line);
    if (text.starts_with("VmRSS:")) resident = bytes(text.substr(6));
    else if (text.starts_with("VmHWM:")) high_water = bytes(text.substr(6));
    if (resident && high_water) return ResidentMemory{*resident, *high_water, RssSource::linux_proc_status};
  }
  return {};
}

std::optional<HostTicks> host_cpu_ticks() {
  std::ifstream stats("/proc/stat");
  std::string label;
  std::uint64_t user{}, nice{}, system{}, idle{}, wait{}, irq{}, soft_irq{}, stolen{};
  if (!(stats >> label >> user >> nice >> system >> idle >> wait >> irq >> soft_irq >> stolen)
      || label != "cpu") return {};
  const auto busy = user + nice + system + irq + soft_irq + stolen;
  return HostTicks{busy, busy + idle + wait};
}

}  // namespace ycxx::bench::detail
