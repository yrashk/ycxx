// ι (#313): a reference to function bound to an overload set selects the member of the target
// type; a call through the reference calls it ([over.over], [dcl.init.ref], [expr.call]).
// EXIT: 0
int f(int x) { return x + 1; }
int f(long x) { return static_cast<int>(x) + 100; }
int f(int x, int y) { return x * y; }

int twice(int (&fn)(int), int v) { return fn(fn(v)); }

int main() {
  int (&r)(int) = f;
  int (&s)(long) = f;
  int (&t)(int, int) = (f);
  if (r(1) != 2) return 1;
  if (s(1) != 101) return 2;
  if (t(3, 4) != 12) return 3;
  // A reference parameter is a target too.
  if (twice(f, 5) != 7) return 4;
  // The reference and a pointer obtained from it denote the same function.
  int (*p)(int) = r;
  if (p != &r) return 5;
  int (*q)(int) = f;
  if (p != q) return 6;
  return 0;
}
