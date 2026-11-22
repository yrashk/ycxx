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

  scope_id scope_;
  ownership::affine_state state_;

  token(scope_id scope) noexcept : scope_(scope) {}

 public:
  token(const token&) = delete;
  token(token&& source) noexcept
      : scope_(source.scope_), state_(static_cast<ownership::affine_state&&>(source.state_)) {}

  template<rights_mask Required> requires grants<Granted, Required>
  void require(scope_id scope) const noexcept {
    state_.observe();
    if (scope != scope_) ownership::detail::ownership_violation();
  }

  void release() noexcept { state_.consume(); }
};

}  // namespace ycxx::capabilities
