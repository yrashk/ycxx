// [expr.spaceship] with its neighbours: in templates and their instantiations ([temp.dep.expr]),
// discarded ([expr.context]), unevaluated ([expr.unary.noexcept], [dcl.type.decltype]), as a
// conditional operand ([expr.cond]) and as a namespace-scope initializer.
#include "inputs/compare.hpp"

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

template<class T> constexpr auto compare(T a, T b) { return a <=> b; }
template<class T> constexpr bool less(const T& a, const T& b) { return a < b; }

struct V {
    int v;
    constexpr std::strong_ordering operator<=>(const V& o) const { return v <=> o.v; }
};
template<class T> struct Box {
    T t;
    constexpr auto operator<=>(const Box& o) const { return t <=> o.t; }
};
struct D {
    int x;
    auto operator<=>(const D&) const = default;
};

static_assert(compare(1, 2) < 0 && compare(2.0, 1.0) > 0);
static_assert(same<decltype(compare(2.0, 1.0)), std::partial_ordering>);
static_assert(less(V{1}, V{2}) && compare(V{2}, V{1}) > 0);
static_assert(Box<int>{1} < Box<int>{2} && !(Box<double>{1.0} > Box<double>{2.0}));
static_assert(less(D{1}, D{2}) && !less(D{2}, D{2}));
static_assert(noexcept(1 <=> 2));
static_assert(same<decltype(1 <=> 2), std::strong_ordering>);

constexpr auto constant = 1 <=> 2;
static_assert(constant < 0);

int count = 0;
int next() { return ++count; }
std::strong_ordering dynamic = next() <=> 0;  // dynamic initialization

constexpr std::strong_ordering choose(bool b, int x) {
    std::strong_ordering r = b ? x <=> 0 : 0 <=> x;
    return r;
}
static_assert(choose(true, 1) > 0 && choose(false, 1) < 0);

int main() {
    if (dynamic <= 0 || count != 1) return 1;
    (void)(next() <=> 0);  // discarded: the operands are still evaluated
    next() <=> 0;          // likewise without the cast
    if (count != 3) return 2;
    if (compare(3, 3) != 0 || !less(D{0}, D{1}) || choose(true, -1) > 0) return 3;
    return 0;
}
