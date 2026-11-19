// θ step 1 (#279), spec-only: `auto&&` is a forwarding reference: an lvalue initializer deduces an lvalue
// reference, an rvalue an rvalue reference ([dcl.type.auto.deduct], [temp.deduct.call]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int make() { return 8; }

int main() {
  int x = 1;
  auto&& l = x;
  static_assert(same<decltype(l), int&>::value);
  l = 2;
  if (x != 2) return 1;
  const int c = 3;
  auto&& lc = c;
  static_assert(same<decltype(lc), const int&>::value);
  auto&& r = make();
  static_assert(same<decltype(r), int&&>::value);
  r = 9;
  if (r != 9) return 2;
  auto&& r2 = 40 + 2;
  static_assert(same<decltype(r2), int&&>::value);
  if (r2 != 42) return 3;
  if (lc != 3) return 4;
  return 0;
}
