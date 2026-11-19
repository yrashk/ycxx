// θ step 1 (#279), spec-only: `auto x{e}` with a single element and `auto x(e)` deduce the type of e, as
// `auto x = e` does ([dcl.type.auto.deduct], [dcl.init.list]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto a{5};
  static_assert(same<decltype(a), int>::value);
  auto b(6L);
  static_assert(same<decltype(b), long>::value);
  char ch = 'z';
  auto c{ch};
  static_assert(same<decltype(c), char>::value);
  auto d(ch);
  static_assert(same<decltype(d), char>::value);
  const int k = 2;
  auto e{k};
  static_assert(same<decltype(e), int>::value);
  if (a != 5) return 1;
  if (b != 6) return 2;
  if (c != 'z' || d != 'z') return 3;
  if (e != 2) return 4;
  return 0;
}
