// [class.spaceship]/3, [except.spec]: the result is a named local in the nonzero test.
#include "inputs/compare.hpp"
struct R {
    int payload;
    constexpr R(std::strong_ordering value) noexcept : payload(value < 0 ? -1 : value > 0 ? 1 : 0) {}
    constexpr bool operator!=(int rhs) & noexcept { return payload != rhs; }
    constexpr bool operator!=(int) && noexcept(false) { return true; }
    constexpr bool operator==(int rhs) const noexcept { return payload == rhs; }
};
struct B {
    int first;
    int second;
    constexpr R operator<=>(const B&) const = default;
    constexpr bool operator==(const B&) const = default;
};
static_assert(noexcept(B{} <=> B{}));
static_assert(!noexcept(R(std::strong_ordering::equal) != 0));
static_assert((B{1, 2} <=> B{2, 1}).payload == -1);
static_assert((B{1, 2} <=> B{1, 1}).payload == 1);
static_assert((B{1, 2} <=> B{1, 2}).payload == 0);
static_assert(B{1, 2} == B{1, 2});
static_assert(!(B{1, 2} == B{1, 1}));
int main(int argc, char**) {
    R equal = std::strong_ordering::equal;
    static_assert(noexcept(equal != 0));
    const B a{argc, 2}, b{argc, 1};
    return (a <=> b).payload != 1 || a == b || equal != 0;
}
