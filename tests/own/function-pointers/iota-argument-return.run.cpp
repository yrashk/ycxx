// ι (#313): a parameter of pointer-to-function type and the return type of a function returning
// a pointer to function are targets of an overload set ([over.over], [expr.call], [stmt.return],
// [dcl.fct]).
// EXIT: 0
int f(int x) { return x + 1; }
int f(long x) { return static_cast<int>(x) + 2; }
int f(int x, int y) { return x - y; }

int apply(int (*fn)(int), int v) { return fn(v); }
int apply_long(int (*fn)(long), long v) { return fn(v); }
int apply2(int (*fn)(int, int), int a, int b) { return fn(a, b); }

// A function returning a pointer to function, written with the declarator ...
int (*pick_long())(long) { return f; }
// ... and with an alias.
using binary = int (*)(int, int);
binary pick_binary() { return &f; }

int (*pick(bool unary))(int) {
  if (unary) return f;
  return nullptr;
}

int main() {
  if (apply(f, 1) != 2) return 1;
  if (apply_long(f, 1) != 3) return 2;
  if (apply2(&f, 7, 3) != 4) return 3;
  if (pick_long()(5) != 7) return 4;
  if (pick_binary()(5, 1) != 4) return 5;
  if (pick(true)(9) != 10) return 6;
  if (pick(false) != nullptr) return 7;
  // The returned pointer can be called through `*` as well.
  if ((*pick_long())(0) != 2) return 8;
  return 0;
}
