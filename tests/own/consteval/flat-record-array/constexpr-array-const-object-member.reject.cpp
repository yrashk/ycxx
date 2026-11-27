// [basic.type.qualifier], [dcl.type.cv], [expr.const.core]
// XFAIL: gcc GCC 16.2 accepts modification of the non-mutable member of a const element.
struct R { int x; };

consteval int f() {
  const R a[2] = {{3}, {5}};
  int& x = const_cast<int&>(a[1].x);
  x = 7;
  return x;
}

static_assert(f() == 7); //! error
