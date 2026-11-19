// θ step 3 (#281), spec-only: a `const auto&` parameter of a generic lambda binds to lvalues without a
// copy and to rvalues of any type; the invented template parameter is deduced without the const
// ([expr.prim.lambda.closure], [dcl.fct], [temp.deduct.call], [dcl.init.ref]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct Pair { int a; int b; };

int main() {
  auto addr = [](const auto& x) { return &x; };
  int i = 3;
  if (addr(i) != &i) return 1;
  if (!same<decltype(addr(i)), const int*>::value) return 2;
  Pair p{1, 2};
  if (addr(p) != &p) return 3;
  if (!same<decltype(addr(p)), const Pair*>::value) return 4;
  auto value = [](const auto& x) { return x; };
  if (value(42) != 42) return 5;
  if (value('z') != 'z') return 6;
  if (!same<decltype(value('z')), char>::value) return 7;
  if (value(Pair{4, 5}).b != 5) return 8;
  auto sum = [](const auto& q) { return q.a + q.b; };
  if (sum(p) != 3 || sum(Pair{10, 20}) != 30) return 9;
  return 0;
}
