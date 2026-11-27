// [dcl.type.cv], [expr.const.core]
struct R { const int x; };

consteval int f() {
  R a[2] = {{3}, {5}};
  int& x = const_cast<int&>(a[1].x);
  x = 7; //! error[gcc]
  return x;
}

static_assert(f() == 7); //! error
