#pragma once

namespace ycxx::effects {

// The callable belongs to the enclosing scope and outlives this handler.
template<class Tag, class Implementation>
class scoped_handler {
  Implementation& implementation_;

 public:
  using effect = Tag;

  scoped_handler(Implementation& implementation) noexcept : implementation_(implementation) {}
  scoped_handler(const scoped_handler&) = delete;
  scoped_handler(scoped_handler&&) = delete;

  template<class... Args>
  decltype(auto) perform(Args&&... args) {
    return implementation_(static_cast<Args&&>(args)...);
  }
};

}  // namespace ycxx::effects
