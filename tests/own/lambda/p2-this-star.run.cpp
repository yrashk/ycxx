// θ step 2 (#280), spec-only: `[*this]` captures the object by copy: later changes to the object are
// not seen, and with `mutable` the copy can change while the object stays ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure]).
struct S {
  int m;
  int snapshot() {
    auto f = [*this] { return m; };
    m = 50;
    return f();
  }
  int scratch() {
    auto f = [*this]() mutable { m += 1000; return m; };
    return f();
  }
};

int main() {
  S s{5};
  if (s.snapshot() != 5) return 1;
  if (s.m != 50) return 2;
  if (s.scratch() != 1050) return 3;
  if (s.m != 50) return 4;
  return 0;
}
