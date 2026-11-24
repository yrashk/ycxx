// [expr.ref], [over.oper.general]: a class member access can name an operator function by its
// operator-function-id, `x.operator<=>(y)` or `p->operator==(y)`.
#include "inputs/compare.hpp"

struct A {
    int v;
    constexpr bool operator==(const A& o) const { return v == o.v; }
    constexpr std::strong_ordering operator<=>(const A& o) const { return v <=> o.v; }
    constexpr int operator()(int i) const { return v + i; }
};

template<class T> concept Member = requires(const T& t) { t.operator<=>(t); };
static_assert(Member<A>);
static_assert(!Member<int>);
static_assert(A{1}.operator<=>(A{2}) < 0);
static_assert(A{3}.operator()(4) == 7);

int main() {
    A a{1}, b{1};
    const A* p = &a;
    return a.operator==(b) && p->operator<=>(b) == 0 ? 0 : 1;
}
