#pragma once

#ifndef __ycxx__
#include <cstdlib>
#endif

namespace ycxx::ownership {

namespace detail {
[[noreturn]] inline void ownership_violation() noexcept {
#ifdef __ycxx__
  __builtin_trap();
#else
  std::abort();
#endif
}
}  // namespace detail

// A guard for a portable resource adapter. Its lifetime tracks the adapter's obligation.
template<bool Linear>
class runtime_state {
 public:
  runtime_state() noexcept : live_(true) {}
  runtime_state(const runtime_state&) = delete;
  runtime_state(runtime_state&& source) noexcept : live_(true) { source.consume(); }

  ~runtime_state() noexcept {
    if (Linear && live_) detail::ownership_violation();
  }

  void observe() const noexcept {
    if (!live_) detail::ownership_violation();
  }

  void consume() noexcept {
    observe();
    live_ = false;
  }

 private:
  bool live_;
};

using affine_state = runtime_state<false>;
using linear_state = runtime_state<true>;

}  // namespace ycxx::ownership
