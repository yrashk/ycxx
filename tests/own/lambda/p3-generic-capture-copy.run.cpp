// θ step 3 (#281), spec-only: a generic lambda's copy captures are members of its closure type, shared
// by every specialization of its function call operator; later changes to the variable are not seen
// ([expr.prim.lambda.capture], [expr.prim.lambda.closure]).
int main() {
  int base = 100;
  char off = 1;
  auto add = [base, off](auto x) { return base + off + x; };
  base = 0;
  if (add(1) != 102) return 1;
  if (add('\2') != 103) return 2;
  if (add(10L) != 111L) return 3;
  auto all = [=](auto x) { return base * 10 + x; };
  base = 5;
  if (all(3) != 3) return 4;
  if (all(3L) != 3L) return 5;
  return 0;
}
