// [over.match.oper]/3.4, /8: `x < y`, `x <= y`, `x > y` and `x >= y` use `operator<=>` when no
// operator of their own is viable, as `(x <=> y) @ 0`, or with the operands reversed, as
// `0 @ (y <=> x)`.
#include "inputs/compare.hpp"

struct V {
    int v;
    constexpr std::strong_ordering operator<=>(const V& o) const { return v <=> o.v; }
};

struct W {
    int v;
    friend constexpr std::strong_ordering operator<=>(W w, int i) { return w.v <=> i; }
};

// A declared `operator<` is used instead of the rewritten form.
struct L {
    int v;
    constexpr std::strong_ordering operator<=>(const L&) const { return std::strong_ordering::greater; }
    constexpr bool operator<(const L&) const { return true; }
};

static_assert(V{1} < V{2} && V{1} <= V{2} && !(V{1} > V{2}) && !(V{1} >= V{2}));
static_assert(V{2} <= V{2} && V{2} >= V{2});
static_assert(W{1} < 2 && W{3} > 2 && W{2} <= 2 && W{2} >= 2);
static_assert(2 > W{1} && 2 < W{3} && 2 >= W{2} && 2 <= W{2});  // reversed
static_assert(L{1} < L{2} && L{1} > L{2});

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;
static_assert(same<decltype(V{1} < V{2}), bool>);
static_assert(same<decltype(2 < W{1}), bool>);

int main() {
    V a{1}, b{2};
    W w{5};
    if (!(a < b) || b < a || !(a <= a) || a > b || !(b >= a)) return 1;
    if (!(w < 6) || !(4 < w) || w > 5 || 5 > w) return 2;
    return 0;
}
