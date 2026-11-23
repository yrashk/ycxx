#pragma once

#include <ycxx/ownership/runtime.hpp>

namespace ycxx::effects {

// One scope's counter, shared by reference between its mediated operations.
template<class Tag, unsigned long long Maximum> requires (Maximum > 0)
class budget {
  [[maybe_unused]] unsigned long long remaining_;

 public:
#if YCXX_POLICIES_RUNTIME_CHECKS
  budget() noexcept : remaining_(Maximum) {}
#else
  budget() noexcept {}
#endif
  budget(const budget&) = delete;
  budget(budget&&) = delete;

  void spend() noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    if (remaining_ == 0) ownership::detail::ownership_violation();
    --remaining_;
#endif
  }
};

}  // namespace ycxx::effects
