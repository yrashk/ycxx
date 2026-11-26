// Local classes (#117), spec-only: two sibling blocks of one function define different local classes
// with the same name; each block sees its own, and an inner block's class hides an outer one
// ([class.local], [basic.scope.block], [basic.scope.hiding]).
int main() {
  int r1 = 0, r2 = 0, r3 = 0;
  {
    struct T {
      char c;
      int f() const { return 1; }
    };
    T t{'x'};
    if (sizeof(T) != 1) return 1;
    r1 = t.f() + t.c;
  }
  {
    struct T {
      int a, b, c;
      int f() const { return a + b + c; }
    };
    T t{1, 2, 3};
    if (sizeof(T) != 3 * sizeof(int)) return 2;
    r2 = t.f();
    {
      struct T { long long v; };
      T inner{40};
      if (sizeof(T) != sizeof(long long)) return 3;
      r3 = static_cast<int>(inner.v);
    }
    if (sizeof(T) != 3 * sizeof(int)) return 4;
  }
  if (r1 != 1 + 'x') return 5;
  if (r2 != 6) return 6;
  if (r3 != 40) return 7;
  return 0;
}
