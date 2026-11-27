// #340: a static data member template named through an object, `object.v<args>` and
// `pointer->v<args>`, whose object expression is evaluated and discarded ([expr.ref]/2,
// [temp.mem]); and initializers with lambdas, whose closure types the member template's
// specializations own, with a deduced type ([expr.prim.lambda.closure], [dcl.type.auto.deduct]).
// EXIT: 0
struct A {
  template <class T> static constexpr int v = sizeof(T);
  template <class T> inline static int n = 0;
};
template <class C> struct S {
  template <class T> static constexpr auto f = [](T x) { return x + C(1); };
  template <class T> static constexpr int sq = [](int x) { return x * x; }(sizeof(T));
};
struct N {
  template <class T> static constexpr int sq = [](int x) { return x * x; }(sizeof(T));
};
int evaluated = 0;
A& object() {
  ++evaluated;
  static A a;
  return a;
}
static_assert(S<int>::sq<long> == 64 && N::sq<int> == 16);
int main() {
  A a;
  A* p = &a;
  if (a.v<int> + p->v<char> != 5) return 1;
  object().n<long> = 3;
  if (evaluated != 1 || A::n<long> != 3 || p->n<long> != 3) return 2;
  return S<int>::f<long>(2) == 3 ? 0 : 3;
}
