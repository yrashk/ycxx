// [class.compare.default], [except.spec]: a defaulted three-way comparison without a
// noexcept-specifier is potentially-throwing when an expression of its implicit definition is,
// including the `vi != 0` that tests each subobject's result ([class.spaceship]/3).
// XFAIL: gcc GCC 16 tests each result with `vi == 0`, not `vi != 0`, so it deletes S's operator
#include "inputs/compare.hpp"

struct Ord {
    int v;
    constexpr Ord(std::strong_ordering o) noexcept : v(o < 0 ? -1 : o > 0 ? 1 : 0) {}
    friend constexpr bool operator!=(Ord o, int) { return o.v != 0; }  // potentially-throwing
};
struct S {
    int x;
    Ord operator<=>(const S&) const = default;
};
struct T {
    int x;
    auto operator<=>(const T&) const = default;
};
static_assert(!noexcept(S{} <=> S{}));
static_assert(noexcept(T{} <=> T{}));
