// θ step 2 (#280), spec-only: unary `+` on a captureless closure object applies the conversion to pointer
// to function ([expr.prim.lambda.closure], [expr.unary.op], [over.built]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto p = +[](int a) { return a + 1; };
  static_assert(same<decltype(p), int (*)(int)>::value);
  if (p(41) != 42) return 1;
  auto l = [](char c) -> long { return c; };
  auto q = +l;
  static_assert(same<decltype(q), long (*)(char)>::value);
  if (q('a') != 97) return 2;
  if ((+[] { return 3; })() != 3) return 3;
  return 0;
}
