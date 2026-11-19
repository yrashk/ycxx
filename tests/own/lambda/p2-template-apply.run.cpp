// θ step 2 (#280), spec-only: closure objects passed by value and by reference to function templates
// whose template parameter deduces the closure type, and called there ([temp.deduct.call],
// [expr.prim.lambda.closure], [expr.call]).
template<class F> int apply(F f, int x) { return f(x); }
template<class F> int twice(F& f) { f(); return f(); }
template<class F> int callConst(const F& f, int a, int b) { return f(a, b); }

int main() {
  if (apply([](int v) { return v * 3; }, 4) != 12) return 1;
  int k = 10;
  if (apply([k](int v) { return v + k; }, 1) != 11) return 2;
  if (apply([&k](int v) { k = v; return 0; }, 77) != 0 || k != 77) return 3;
  auto counter = [n = 0]() mutable { return ++n; };
  if (twice(counter) != 2) return 4;
  if (counter() != 3) return 5;
  if (callConst([](int a, int b) { return a - b; }, 9, 4) != 5) return 6;
  return 0;
}
