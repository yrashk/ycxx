// [expr.add], [basic.compound]
struct R { int x, y; };

consteval int f() {
  R a[2] = {{1, 2}, {3, 4}};
  int* p = &a[0].y + 1;
  return *(p - 1);
}

static_assert(f() == 2);
