// [class.spaceship]/2-3: a defaulted three-way comparison of subobjects of several comparison
// categories returns their common category, each subobject's result converted to it; a declared
// return type R converts each result to R.
#include "inputs/compare.hpp"

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

struct F {
    int i;
    double d;
    friend constexpr auto operator<=>(const F&, const F&) = default;
};
static_assert(same<decltype(F{} <=> F{}), std::partial_ordering>);
static_assert((F{1, 2.0} <=> F{1, 2.5}) < 0);
static_assert((F{0, 2.0} <=> F{1, 2.5}) < 0);

struct W {
    long v;
    std::weak_ordering operator<=>(const W&) const = default;
};
static_assert(same<decltype(W{} <=> W{}), std::weak_ordering>);
static_assert((W{1} <=> W{2}) < 0);

int main() {
    F a{1, 2.0}, b{1, 1.0};
    W w{3}, v{3};
    return (a <=> b) > 0 && (w <=> v) == 0 ? 0 : 1;
}
