// [expr.add], [expr.unary.op], [expr.const.core]
struct R { int x, y; };

consteval int f() {
  R a[2] = {{1, 2}, {3, 4}};
  int* p = &a[0].y + 1;
  return *p; //! error[gcc]
}

static_assert(f() == 2); //! error
