#pragma once

#include <ycxx/ownership/runtime.hpp>

namespace ycxx::effects {

// One scope's counter, shared by reference between its mediated operations.
template<class Tag, unsigned long long Maximum> requires (Maximum > 0)
class budget {
  unsigned long long remaining_;

 public:
  budget() noexcept : remaining_(Maximum) {}
  budget(const budget&) = delete;
  budget(budget&&) = delete;

  void spend() noexcept {
    if (remaining_ == 0) ownership::detail::ownership_violation();
    --remaining_;
  }
};

}  // namespace ycxx::effects
