// θ step 3 (#281), spec-only: each specialization of a generic lambda's function call operator deduces
// its own return type ([expr.prim.lambda.closure], [dcl.spec.auto.general], [dcl.fct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct Wrapped { int v; };

int main() {
  auto deref = [](auto p) { return *p; };
  int i = 3;
  char c = 'c';
  if (!same<decltype(deref(&i)), int>::value) return 1;
  if (!same<decltype(deref(&c)), char>::value) return 2;
  auto ref = [](auto& x) -> auto& { return x; };
  if (!same<decltype(ref(i)), int&>::value) return 3;
  ref(i) = 9;
  if (i != 9) return 4;
  auto wrap = [](auto x) { return Wrapped{static_cast<int>(x)}; };
  if (!same<decltype(wrap(1L)), Wrapped>::value) return 7;
  if (wrap('\5').v != 5) return 8;
  auto none = [](auto& x) { x = 0; };
  if (!same<decltype(none(i)), void>::value) return 9;
  none(i);
  if (i != 0) return 10;
  return 0;
}
