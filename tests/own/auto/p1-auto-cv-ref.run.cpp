// θ step 1 (#279), spec-only: `const auto`, `auto&` and `const auto&` at block scope; `auto&` keeps the
// referent's cv-qualification and binds to the object ([dcl.spec.auto.general],
// [dcl.type.auto.deduct], [dcl.init.ref]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  int x = 1;
  const auto a = x;
  static_assert(same<decltype(a), const int>::value);
  auto& r = x;
  static_assert(same<decltype(r), int&>::value);
  r = 10;
  if (x != 10) return 1;
  const auto& cr = x;
  static_assert(same<decltype(cr), const int&>::value);
  x = 11;
  if (cr != 11) return 2;
  const int k = 4;
  auto& rk = k;
  static_assert(same<decltype(rk), const int&>::value);
  if (rk != 4) return 3;
  const auto& tmp = 5 + 6;
  static_assert(same<decltype(tmp), const int&>::value);
  if (tmp != 11) return 4;
  if (a != 1) return 5;
  return 0;
}
