// θ step 3 (#281), spec-only: a generic lambda's function call operator is a template with one invented
// type parameter per `auto` parameter; each call with a different argument type uses its own
// specialization, whose deduced return type follows the argument ([expr.prim.lambda.closure],
// [dcl.fct], [temp.deduct.call], [dcl.spec.auto.general]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto id = [](auto x) { return x; };
  int i = 7;
  char c = 'a';
  long l = 40000L;
  bool b = true;
  if (!same<decltype(id(i)), int>::value) return 1;
  if (!same<decltype(id(c)), char>::value) return 2;
  if (!same<decltype(id(l)), long>::value) return 3;
  if (!same<decltype(id(b)), bool>::value) return 4;
  if (!same<decltype(id(&i)), int*>::value) return 5;
  if (id(i) != 7) return 6;
  if (id(c) != 'a') return 7;
  if (id(l) != 40000L) return 8;
  if (!id(b)) return 9;
  if (*id(&i) != 7) return 10;
  auto twice = [](auto x) { return x + x; };
  if (!same<decltype(twice(c)), int>::value) return 11;
  if (!same<decltype(twice(l)), long>::value) return 12;
  if (twice(3) != 6 || twice(l) != 80000L) return 13;
  return 0;
}
