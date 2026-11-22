// #218: `__reference_converts_from_temporary(T, U)` and `__reference_constructs_from_temporary(T, U)`
// ([meta.rel]): whether a reference T initialized from VAL<U> binds to a temporary. Arithmetic
// operands (a materialized prvalue, a converted value, a direct binding), reference-related
// classes, non-reference targets and void; and dependent operands in a function template body, as
// libycxx's invoke.hpp has them.
// EXIT: 0
struct B {};
struct D : B {};
#define C(T, F) __reference_converts_from_temporary(T, F)
#define K(T, F) __reference_constructs_from_temporary(T, F)
static_assert(C(const int&, int) && C(int&&, int) && !C(int&, int));
static_assert(C(const int&, long) && C(const long&, int&) && !C(const int&, int&) && !C(const int&, const int&));
static_assert(!C(int, int) && !C(int, long));
static_assert(C(const B&, B) && C(B&&, B) && !C(const B&, B&) && !C(const B&, D&) && C(const B&, D) && !C(B&, B));
static_assert(!C(int&&, int&&) && !C(const int&, int&&) && C(const double&, int&&));
static_assert(K(const int&, int) && K(const B&, D) && !K(const int&, int&) && !K(int, int));
static_assert(!C(void, void) && !C(const int&, void));

template<class R, class F> constexpr bool dangles() { return __reference_converts_from_temporary(R, F); }
int main() {
  if (!dangles<const int&, long>() || dangles<const int&, int&>() || !dangles<const B&, D>()) return 1;
  return 0;
}
