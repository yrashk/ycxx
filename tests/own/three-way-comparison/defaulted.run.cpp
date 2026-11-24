// [class.spaceship], [class.compare.default]: a defaulted three-way comparison operator compares
// the base class subobjects, then the members, in order; with a return type of `auto` its type is
// their common comparison category ([class.spaceship]/2). The relational operators are rewritten
// from it.
#include "inputs/compare.hpp"

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

struct P {
    int x, y;
    auto operator<=>(const P&) const = default;
};
static_assert(same<decltype(P{} <=> P{}), std::strong_ordering>);
static_assert((P{1, 2} <=> P{1, 3}) < 0);
static_assert((P{2, 0} <=> P{1, 3}) > 0);
static_assert((P{1, 2} <=> P{1, 2}) == 0);
static_assert(P{1, 2} < P{1, 3} && P{1, 3} >= P{1, 2});

// A floating member makes the category partial.
struct F {
    double d;
    friend constexpr auto operator<=>(const F&, const F&) = default;
};
static_assert(same<decltype(F{} <=> F{}), std::partial_ordering>);
static_assert((F{2.0} <=> F{2.5}) < 0);
static_assert((F{__builtin_nan("")} <=> F{2.5}) == std::partial_ordering::unordered);

// A declared return type is used as given.
struct S {
    long v;
    std::strong_ordering operator<=>(const S&) const = default;
};
static_assert(same<decltype(S{} <=> S{}), std::strong_ordering>);

struct Empty {
    auto operator<=>(const Empty&) const = default;
};
static_assert((Empty{} <=> Empty{}) == std::strong_ordering::equal);

// Base class subobjects first, then the class's own members; class members by their own `<=>`.
struct Base {
    int b;
    auto operator<=>(const Base&) const = default;
};
struct Derived : Base {
    int d;
    auto operator<=>(const Derived&) const = default;
};
struct Outer {
    P p;
    int arr[2];
    auto operator<=>(const Outer&) const = default;
};
static_assert(same<decltype(Outer{} <=> Outer{}), std::strong_ordering>);

int main() {
    Derived a, b;
    a.b = 1; a.d = 9;
    b.b = 2; b.d = 0;
    if ((a <=> b) >= 0 || !(a < b)) return 1;
    b.b = 1;
    if ((a <=> b) <= 0 || !(a > b)) return 2;
    Outer o{{1, 2}, {3, 4}}, q{{1, 2}, {3, 5}};
    if ((o <=> q) >= 0 || !(o < q)) return 3;
    q.arr[1] = 4;
    if ((o <=> q) != 0 || o < q) return 4;
    return 0;
}
