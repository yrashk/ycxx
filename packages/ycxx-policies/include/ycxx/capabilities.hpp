#pragma once

#include <ycxx/ownership/runtime.hpp>

namespace ycxx::capabilities {

using rights_mask = unsigned long long;
using scope_id = unsigned long long;

template<rights_mask Granted, rights_mask Required>
concept grants = Required != 0 && (Granted & Required) == Required;

// The issuer defines the meaning of the scope identifiers and rights in its domain.
template<class Issuer, rights_mask Granted>
class token {
  friend Issuer;

  [[maybe_unused]] scope_id scope_;
  [[maybe_unused]] ownership::affine_state state_;

#if YCXX_POLICIES_RUNTIME_CHECKS
  token(scope_id scope) noexcept : scope_(scope) {}
#else
  token(scope_id) noexcept {}
#endif

 public:
  token(const token&) = delete;
#if YCXX_POLICIES_RUNTIME_CHECKS
  token(token&& source) noexcept
      : scope_(source.scope_), state_(static_cast<ownership::affine_state&&>(source.state_)) {}
#else
  token(token&&) noexcept {}
#endif

  template<rights_mask Required> requires grants<Granted, Required>
  void require([[maybe_unused]] scope_id scope) const noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    state_.observe();
    if (scope != scope_) ownership::detail::ownership_violation();
#endif
  }

  void release() noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    state_.consume();
#endif
  }
};

}  // namespace ycxx::capabilities
