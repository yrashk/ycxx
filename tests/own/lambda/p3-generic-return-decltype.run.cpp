// θ step 3 (#281), spec-only (split from p3-generic-return-types): a trailing return type that
// depends on a generic lambda's parameters ([expr.prim.lambda.closure], [dcl.type.decltype]).
// XFAIL: ycxx decltype of a type-dependent expression in a declared type, #199
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto trailing = [](auto x) -> decltype(x + 1L) { return x + 1L; };
  if (!same<decltype(trailing('a')), long>::value) return 5;
  if (trailing(1) != 2L) return 6;
  return 0;
}
