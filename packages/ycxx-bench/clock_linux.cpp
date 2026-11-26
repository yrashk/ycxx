module;
#include <time.h>
#include <unistd.h>
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

std::uint64_t rss_bytes(long amount) { return std::uint64_t(amount) * 1024; }

std::optional<std::uint64_t> resident_memory() {
  static const auto page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return {};
  std::ifstream pages("/proc/self/statm");
  std::uint64_t virtual_pages{}, resident_pages{};
  if (!(pages >> virtual_pages >> resident_pages)) return {};
  return resident_pages * std::uint64_t(page_size);
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
