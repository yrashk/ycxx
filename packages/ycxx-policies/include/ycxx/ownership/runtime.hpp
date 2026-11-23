#pragma once

#ifndef YCXX_POLICIES_RUNTIME_CHECKS
#define YCXX_POLICIES_RUNTIME_CHECKS 1
#endif

#if YCXX_POLICIES_RUNTIME_CHECKS
// Use the same definition when linking YCXX, GCC, and Clang objects together.
#ifdef __has_builtin
#if __has_builtin(__builtin_trap)
#define YCXX_POLICIES_HAS_TRAP 1
#endif
#endif

#ifndef YCXX_POLICIES_HAS_TRAP
#include <cstdlib>
#endif
#endif

namespace ycxx::ownership {

static_assert(YCXX_POLICIES_RUNTIME_CHECKS == 0 || YCXX_POLICIES_RUNTIME_CHECKS == 1,
              "YCXX_POLICIES_RUNTIME_CHECKS must be 0 or 1");

#if YCXX_POLICIES_RUNTIME_CHECKS
namespace detail {
[[noreturn]] inline void ownership_violation() noexcept {
#ifdef YCXX_POLICIES_HAS_TRAP
  __builtin_trap();
#else
  std::abort();
#endif
}
}  // namespace detail
#endif

// A guard for a portable resource adapter. Its lifetime tracks the adapter's obligation.
template<bool Linear>
class runtime_state {
 public:
#if YCXX_POLICIES_RUNTIME_CHECKS
  runtime_state() noexcept : live_(true) {}
  runtime_state(runtime_state&& source) noexcept : live_(true) { source.consume(); }
#else
  runtime_state() noexcept {}
  runtime_state(runtime_state&&) noexcept {}
#endif
  runtime_state(const runtime_state&) = delete;

  ~runtime_state() noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    if (Linear && live_) detail::ownership_violation();
#endif
  }

  void observe() const noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    if (!live_) detail::ownership_violation();
#endif
  }

  void consume() noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    observe();
    live_ = false;
#endif
  }

 private:
  [[maybe_unused]] bool live_;
};

using affine_state = runtime_state<false>;
using linear_state = runtime_state<true>;

}  // namespace ycxx::ownership

#undef YCXX_POLICIES_HAS_TRAP
