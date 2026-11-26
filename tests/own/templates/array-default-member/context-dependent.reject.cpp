// #1418 review, #1459 [expr.const], [stmt.if]/5, [class.mem.general]: a runtime initialization
// is not manifestly constant-evaluated, so an `if consteval` in a default member initializer takes
// its runtime edge. ycxx folds these defaults in constant mode, so it rejects the runtime uses
// instead of reading the constant-mode value; constant initialization folds them.
// XFAIL: clang,gcc they evaluate the defaults at run time (#1459)
constexpr bool in_constant() {
  if consteval {
    return true;
  } else {
    return false;
  }
}
constexpr int pick() { return in_constant() ? 1 : 2; }

template <int N> struct V { int x = pick(); };    // a specialization's scalar
template <int N> struct A { int w[N] = {pick()}; };  // a specialization's array
struct P { int w[2] = {pick(), 0}; };             // an ordinary class's array
struct B { int base = 0; };
struct D : B { int x = pick(); };                 // an ordinary scalar without replay
struct S { int x = pick(); int y; };              // replayed at run time: accepted

constexpr V<1> constant_v{};
static_assert(constant_v.x == 1);

int main() {
  V<1> v;  //! error
  A<2> a;  //! error
  P p;     //! error
  D d;     //! error
  S s;
  return v.x + a.w[0] + p.w[0] + d.x + s.x;
}
