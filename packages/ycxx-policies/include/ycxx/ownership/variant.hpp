#pragma once

#include <ycxx/ownership/runtime.hpp>

namespace ycxx::ownership {

struct first_t {};
struct second_t {};

namespace detail {
template<class First, class Second>
union variant_storage {
  First first;
  Second second;

  variant_storage(first_t, First&& value) noexcept : first(static_cast<First&&>(value)) {}
  variant_storage(second_t, Second&& value) noexcept : second(static_cast<Second&&>(value)) {}
  ~variant_storage() noexcept {}
};

// End the source alternative's lifetime after constructing the return object.
template<class Member>
struct variant_member_cleanup {
  Member& value;
  ~variant_member_cleanup() noexcept { value.~Member(); }
};
}  // namespace detail

// A local choice owns exactly one active value until that value is taken.
template<class First, class Second>
class variant {
  enum class active { first_selected, second_selected, empty };

  active active_;
  detail::variant_storage<First, Second> storage_;

  void require_active([[maybe_unused]] active expected) const noexcept {
#if YCXX_POLICIES_RUNTIME_CHECKS
    if (active_ != expected) detail::ownership_violation();
#endif
  }

 public:
  variant(first_t side, First&& value) noexcept : active_(active::first_selected), storage_(side, static_cast<First&&>(value)) {
    static_assert(noexcept(First(static_cast<First&&>(value))));
  }

  variant(second_t side, Second&& value) noexcept : active_(active::second_selected), storage_(side, static_cast<Second&&>(value)) {
    static_assert(noexcept(Second(static_cast<Second&&>(value))));
  }

  variant(const variant&) = delete;
  variant(variant&&) = delete;

  ~variant() noexcept {
    if (active_ == active::first_selected) storage_.first.~First();
    if (active_ == active::second_selected) storage_.second.~Second();
  }

  bool holds_first() const noexcept { return active_ == active::first_selected; }
  bool holds_second() const noexcept { return active_ == active::second_selected; }

  const First& first() const noexcept {
    require_active(active::first_selected);
    return storage_.first;
  }

  const Second& second() const noexcept {
    require_active(active::second_selected);
    return storage_.second;
  }

  First take_first() noexcept {
    require_active(active::first_selected);
    detail::variant_member_cleanup<First> cleanup{storage_.first};
    active_ = active::empty;
    return First(static_cast<First&&>(storage_.first));
  }

  Second take_second() noexcept {
    require_active(active::second_selected);
    detail::variant_member_cleanup<Second> cleanup{storage_.second};
    active_ = active::empty;
    return Second(static_cast<Second&&>(storage_.second));
  }
};

}  // namespace ycxx::ownership
