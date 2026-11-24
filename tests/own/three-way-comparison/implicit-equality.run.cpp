// [class.compare.default]/5: a class that declares a defaulted three-way comparison operator
// function and no `operator==` gets an implicitly declared defaulted `operator==`, a member or a
// friend as the `<=>` is.
#include "inputs/compare.hpp"

struct P {
    int x, y;
    auto operator<=>(const P&) const = default;
};
static_assert(P{1, 2} == P{1, 2} && P{1, 2} != P{1, 3});

struct F {
    int v;
    friend constexpr auto operator<=>(const F&, const F&) = default;
};
static_assert(F{1} == F{1} && F{1} != F{2});

int main() {
    P p{1, 2}, r{1, 2};
    F f{3}, g{4};
    return p == r && !(p != r) && f != g ? 0 : 1;
}
