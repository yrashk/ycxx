// [stmt.if], [expr.const], [dcl.constexpr]: `if consteval` takes its first branch during
// constant evaluation; its compound-statement is an immediate function context, so it may call
// consteval functions with the enclosing function's parameters.
consteval int triple(int n) { return n * 3; }
constexpr int where() {
  if consteval {
    return 1;
  } else {
    return 2;
  }
}
constexpr int negated() {
  if !consteval {
    return 10;
  } else {
    return 20;
  }
}
constexpr int scaled(int n) {
  if consteval {
    return triple(n);
  }
  return n * 3;
}
constexpr int no_else() {
  int r = 5;
  if consteval {
    r = r + 1;
  }
  return r;
}
static_assert(where() == 1);
static_assert(negated() == 20);
static_assert(scaled(4) == 12);
static_assert(no_else() == 6);
int at_runtime(int n) { return where() + negated() + scaled(n) + no_else(); }
