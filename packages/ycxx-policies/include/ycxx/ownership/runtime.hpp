#pragma once

// Use the same definition when linking YCXX, GCC, and Clang objects together.
#ifdef __has_builtin
#if __has_builtin(__builtin_trap)
#define YCXX_POLICIES_HAS_TRAP 1
#endif
#endif

#ifndef YCXX_POLICIES_HAS_TRAP
#include <cstdlib>
#endif

namespace ycxx::ownership {

namespace detail {
[[noreturn]] inline void ownership_violation() noexcept {
#ifdef YCXX_POLICIES_HAS_TRAP
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

#undef YCXX_POLICIES_HAS_TRAP
