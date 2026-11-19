// θ step 2 (#280), spec-only: a lambda in a function template: each specialization has its own closure
// type, and the lambda can use the template parameters ([expr.prim.lambda.closure], [temp.spec],
// [temp.dep]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

template<class T> auto make(T v) {
  return [v](T extra) { return T(v + extra); };
}

template<class T> T twice(T v) {
  auto f = [](T x) -> T { return x + x; };
  return f(v);
}

template<int N> int scaled(int x) {
  return [x] { return x * N; }();
}

int main() {
  auto i = make(3);
  auto l = make(3L);
  static_assert(!same<decltype(i), decltype(l)>::value);
  auto ri = i(4);
  auto rl = l(4);
  static_assert(same<decltype(ri), int>::value);
  static_assert(same<decltype(rl), long>::value);
  if (ri != 7 || rl != 7) return 1;
  if (twice(21) != 42 || twice(2L) != 4L) return 2;
  if (scaled<3>(5) != 15 || scaled<4>(5) != 20) return 3;
  return 0;
}
