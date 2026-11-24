// [class.spaceship]/2: a defaulted three-way comparison's deduced return type is plain `auto`.
// [class.compare.default]: one that would compare a subobject with no usable `<=>` is deleted,
// and selecting it makes the comparison ill-formed.
// XFAIL: gcc GCC 16 accepts a `decltype(auto)` or `const auto` return type here
#include "inputs/compare.hpp"

struct A {
    int x;
    decltype(auto) operator<=>(const A&) const = default;  //! error
};
struct B {
    int x;
    const auto operator<=>(const B&) const = default;  //! error
};

struct N {};
struct Base {};
std::strong_ordering operator<=>(const Base&, const Base&);
struct S : Base {
    N n;
    auto operator<=>(const S&) const = default;
};
bool f(const S& a, const S& b) {
    return (a <=> b) < 0;  //! error
}
