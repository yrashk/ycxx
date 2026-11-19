// θ step 3 (#281), spec-only: init-captures of a generic lambda, by copy and by reference, are usable
// from each specialization of its function call operator ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure]).
int main() {
  int v = 4;
  auto f = [k = v * 2, &r = v](auto x) { r += 1; return k + x; };
  if (f(1) != 9) return 1;
  if (f('\3') != 11) return 2;
  if (f(1L) != 9L) return 3;
  if (v != 7) return 4;
  auto g = [n = 'a'](auto x) { return n + x; };
  if (g(1) != 'b') return 5;
  if (g(1L) != 98L) return 6;
  return 0;
}
