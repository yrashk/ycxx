// θ step 3 (#281), spec-only: a generic lambda returned from a function (by a deduced return type)
// keeps its captured copy, and its function call operator template is instantiated by calls made
// later by the caller ([expr.prim.lambda.closure], [expr.prim.lambda.capture], [dcl.spec.auto.general]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

auto adder(int k) {
  return [k](auto x) { return x + k; };
}
auto multiplier(int k) {
  return [k]<class T>(T x) { return static_cast<T>(x * k); };
}

int main() {
  auto add5 = adder(5);
  auto add7 = adder(7);
  if (add5(1) != 6) return 1;
  if (add7(1L) != 8L) return 2;
  if (!same<decltype(add5(1L)), long>::value) return 3;
  if (add5('\0') != 5) return 4;
  if (!same<decltype(add5), decltype(add7)>::value) return 5;
  if (!same<decltype(add5), decltype(adder(0))>::value) return 9;
  auto triple = multiplier(3);
  if (triple(4) != 12) return 6;
  if (!same<decltype(triple('\1')), char>::value) return 7;
  if (triple('\2') != 6) return 8;
  return 0;
}
