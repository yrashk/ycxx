// θ step 2 (#280), spec-only: each evaluation of a lambda-expression creates a new closure object,
// initializing its copies from the current values ([expr.prim.lambda.general],
// [expr.prim.lambda.capture]).
template<class F> int call(F f) { return f(); }

int main() {
  int total = 0;
  for (int i = 0; i < 4; ++i) {
    auto f = [i] { return i * 10; };
    total += call(f);
  }
  if (total != 60) return 1;
  int sum = 0;
  for (int i = 1; i <= 3; ++i) [&sum, i] { sum += i; }();
  if (sum != 6) return 2;
  return 0;
}
