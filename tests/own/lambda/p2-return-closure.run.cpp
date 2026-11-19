// θ step 2 (#280), spec-only: a function with a deduced return type returns a closure object that is
// called after the function has returned; by-copy captures outlive the function's locals
// ([expr.prim.lambda.capture], [expr.prim.lambda.closure], [dcl.spec.auto.general]).
auto adder(int n) {
  return [n](int x) { return x + n; };
}
auto counter(int start) {
  return [start]() mutable { return start++; };
}
auto pick(int which) {
  int local = which * 2;
  return [=] { return local + which; };
}

int main() {
  auto add5 = adder(5);
  auto add7 = adder(7);
  if (add5(1) != 6 || add7(1) != 8) return 1;
  auto c = counter(3);
  c();
  if (c() != 4) return 2;
  if (pick(4)() != 12) return 3;
  return 0;
}
