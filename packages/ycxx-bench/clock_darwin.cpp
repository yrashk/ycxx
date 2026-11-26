module;
#include <mach/mach.h>
#include <mach/mach_time.h>
module ycxx.bench;
import std;

namespace ycxx::bench::detail {

const mach_timebase_info_data_t& timebase() {
  static const auto info = [] {
    mach_timebase_info_data_t result{};
    if (::mach_timebase_info(&result) != 0)
      throw std::runtime_error("mach_timebase_info failed");
    return result;
  }();
  return info;
}

Nanoseconds now_ns() {
  const auto ticks = ::mach_absolute_time();
  const auto [numerator, denominator] = timebase();
  // Keep the integer result without overflowing a ticks * numerator intermediate.
  return Nanoseconds(ticks / denominator * numerator + (ticks % denominator) * numerator / denominator);
}

std::pair<std::string_view, double> native_clock() {
  const auto [numerator, denominator] = timebase();
  return {"mach_absolute_time", double(numerator) / denominator};
}

std::uint64_t rss_bytes(long amount) { return std::uint64_t(amount); }

std::optional<std::uint64_t> resident_memory() {
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
        reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return {};
  return info.resident_size;
}

std::optional<HostTicks> host_cpu_ticks() {
  host_cpu_load_info_data_t info{};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  const auto host = mach_host_self();
  const auto status = ::host_statistics(host, HOST_CPU_LOAD_INFO,
      reinterpret_cast<host_info_t>(&info), &count);
  ::mach_port_deallocate(mach_task_self(), host);
  if (status != KERN_SUCCESS) return {};
  const auto busy = std::uint64_t(info.cpu_ticks[CPU_STATE_USER])
    + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
  return HostTicks{busy, busy + info.cpu_ticks[CPU_STATE_IDLE]};
}

}  // namespace ycxx::bench::detail
