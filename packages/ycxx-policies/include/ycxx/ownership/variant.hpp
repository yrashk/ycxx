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
}  // namespace detail

// A local choice owns exactly one active value until that value is taken.
template<class First, class Second>
class variant {
  enum class active { first, second, empty };

  active active_;
  detail::variant_storage<First, Second> storage_;

  void require_active(active expected) const noexcept {
    if (active_ != expected) detail::ownership_violation();
  }

 public:
  variant(first_t side, First&& value) noexcept : active_(active::first), storage_(side, static_cast<First&&>(value)) {
    static_assert(noexcept(First(static_cast<First&&>(value))));
  }

  variant(second_t side, Second&& value) noexcept : active_(active::second), storage_(side, static_cast<Second&&>(value)) {
    static_assert(noexcept(Second(static_cast<Second&&>(value))));
  }

  variant(const variant&) = delete;
  variant(variant&&) = delete;

  ~variant() noexcept {
    if (active_ == active::first) storage_.first.~First();
    if (active_ == active::second) storage_.second.~Second();
  }

  bool holds_first() const noexcept { return active_ == active::first; }
  bool holds_second() const noexcept { return active_ == active::second; }

  const First& first() const noexcept {
    require_active(active::first);
    return storage_.first;
  }

  const Second& second() const noexcept {
    require_active(active::second);
    return storage_.second;
  }

  First take_first() noexcept {
    require_active(active::first);
    First owner(static_cast<First&&>(storage_.first));
    active_ = active::empty;
    storage_.first.~First();
    return owner;
  }

  Second take_second() noexcept {
    require_active(active::second);
    Second owner(static_cast<Second&&>(storage_.second));
    active_ = active::empty;
    storage_.second.~Second();
    return owner;
  }
};

}  // namespace ycxx::ownership
