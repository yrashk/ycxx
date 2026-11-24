// [over.match.oper]: `x <=> y` with an operand of class type calls a user-declared
// `operator<=>`, a member or a non-member; with only `operator<=>(y, x)` declared, the
// synthesized candidate `0 <=> (y <=> x)` ([over.match.oper]/3.4, /8).
#include "inputs/compare.hpp"

struct Member {
    int v;
    constexpr std::strong_ordering operator<=>(const Member& o) const { return v <=> o.v; }
};

struct Free {
    double v;
};
constexpr std::partial_ordering operator<=>(const Free& a, const Free& b) { return a.v <=> b.v; }

// Compared with an int only in this order: `1 <=> h` is `0 <=> (h <=> 1)`.
struct Half {
    int v;
    friend constexpr std::strong_ordering operator<=>(Half h, int i) { return h.v <=> i; }
};

// A result type of its own: `<=>` need not return a comparison category.
struct Count {
    int calls;
};
struct Own {
    int v;
    constexpr Count operator<=>(const Own&) const { return Count{v}; }
};

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

static_assert((Member{1} <=> Member{2}) < 0);
static_assert((Member{2} <=> Member{2}) == 0);
static_assert((Free{1.0} <=> Free{0.5}) > 0);
static_assert(same<decltype(Free{} <=> Free{}), std::partial_ordering>);
static_assert((Half{3} <=> 1) > 0);
static_assert((1 <=> Half{3}) < 0);
static_assert((3 <=> Half{3}) == 0);
static_assert(same<decltype(1 <=> Half{3}), std::strong_ordering>);
static_assert((Own{7} <=> Own{0}).calls == 7);

int main() {
    Member a{1}, b{2};
    Half h{5};
    if ((a <=> b) >= 0 || (b <=> a) <= 0) return 1;
    if ((h <=> 9) >= 0 || (9 <=> h) <= 0 || (5 <=> h) != 0) return 2;
    if ((Free{2.0} <=> Free{2.0}) != 0) return 3;
    return 0;
}
