// θ step 3 (#281), spec-only: nested generic lambdas: a generic lambda returns another generic lambda
// that captures the first one's parameter by copy, so `curry(a)(b)(c)` instantiates three function
// call operator templates ([expr.prim.lambda.closure], [expr.prim.lambda.capture]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto add = [](auto a) { return [a](auto b) { return a + b; }; };
  if (add(1)(2) != 3) return 1;
  auto add10 = add(10L);
  if (add10(5) != 15L) return 2;
  if (!same<decltype(add10(5)), long>::value) return 3;
  if (!same<decltype(add('\1')('\1')), int>::value) return 4;
  auto curry3 = [](auto a) {
    return [a](auto b) { return [a, b](auto c) { return a * 100 + b * 10 + c; }; };
  };
  if (curry3(1)(2)(3) != 123) return 5;
  auto partial = curry3(4)('\5');
  if (partial(6) != 456) return 6;
  if (partial(7L) != 457L) return 7;
  auto compose = [](auto f, auto g) { return [f, g](auto x) { return f(g(x)); }; };
  auto inc = [](auto x) { return x + 1; };
  auto dbl = [](auto x) { return x * 2; };
  if (compose(inc, dbl)(5) != 11) return 8;
  if (compose(dbl, inc)(5L) != 12L) return 9;
  return 0;
}
