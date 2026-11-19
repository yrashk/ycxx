// ι (#313): a pointer to function initialized from an overload set picks the member whose type
// is the target's, with or without `&` and redundant parentheses ([over.over], [conv.func],
// [expr.unary.op]).
// EXIT: 0
int f(int) { return 1; }
int f(long) { return 2; }
int f(const char*) { return 3; }
int f(int, int) { return 4; }

int main() {
  int (*a)(long) = f;
  int (*b)(int) = &f;
  int (*c)(const char*) = (f);
  int (*d)(int, int) = (&f);
  int (*const e)(long) = &(f);
  int (*g)(int){f};
  int (*h)(long) = {&f};
  if (a(0) != 2) return 1;
  if (b(0) != 1) return 2;
  if (c("x") != 3) return 3;
  if (d(1, 2) != 4) return 4;
  if (e(7) != 2) return 5;
  if (g(7) != 1) return 6;
  if (h(7) != 2) return 7;
  // Ordinary calls still resolve by the arguments.
  if (f(1) != 1 || f(1L) != 2 || f("") != 3 || f(1, 1) != 4) return 8;
  return 0;
}
