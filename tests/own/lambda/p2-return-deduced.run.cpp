// θ step 2 (#280), spec-only: without a trailing-return-type the return type is deduced from the return
// statements as for `auto`; several returns of one type ([expr.prim.lambda.closure],
// [dcl.spec.auto.general], [dcl.type.auto.deduct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto sign = [](int v) {
    if (v < 0) return -1;
    if (v == 0) return 0;
    return 1;
  };
  if (sign(-4) != -1 || sign(0) != 0 || sign(8) != 1) return 1;
  auto first = [](char a, char b, bool f) {
    if (f) return a;
    return b;
  };
  auto c = first('x', 'y', false);
  static_assert(same<decltype(c), char>::value);
  if (c != 'y') return 2;
  auto big = [](long a) { return a + 1; };
  auto l = big(4);
  static_assert(same<decltype(l), long>::value);
  if (l != 5) return 3;
  int x = 6;
  auto copy = [](int& r) { return r; };
  auto y = copy(x);
  static_assert(same<decltype(y), int>::value);
  return y == 6 ? 0 : 4;
}
