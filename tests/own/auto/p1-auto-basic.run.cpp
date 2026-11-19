// θ step 1 (#279), spec-only: plain `auto` at block scope deduces the initializer's type with top-level cv
// dropped ([dcl.spec.auto.general], [dcl.type.auto.deduct], [temp.deduct.call]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto i = 42;
  static_assert(same<decltype(i), int>::value);
  auto c = 'c';
  static_assert(same<decltype(c), char>::value);
  auto l = 5L;
  static_assert(same<decltype(l), long>::value);
  auto u = 7u;
  static_assert(same<decltype(u), unsigned>::value);
  const int ci = 3;
  auto d = ci;
  static_assert(same<decltype(d), int>::value);
  d = 9;
  if (i != 42) return 1;
  if (c != 'c') return 2;
  if (l != 5) return 3;
  if (u != 7) return 4;
  if (d != 9 || ci != 3) return 5;
  return 0;
}
