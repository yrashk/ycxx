// [over.match.oper]/2, /3.4: rewritten candidates are also considered for enumeration operands.
// A scoped enumeration has no built-in comparison with another type, so the rewritten
// `operator<=>` is the only candidate.
#include "inputs/compare.hpp"

enum class E { a, b };
constexpr std::strong_ordering operator<=>(E e, int i) { return static_cast<int>(e) <=> i; }

static_assert(E::b > 0 && E::a < 1 && !(E::a > 0));
static_assert(5 > E::b && 0 >= E::a);  // reversed
static_assert((5 <=> E::b) > 0);      // synthesized: 0 <=> (E::b <=> 5)
static_assert(E::a < E::b);           // the built-in comparison of E

int main() { return E::a < 1 && 2 > E::b ? 0 : 1; }
