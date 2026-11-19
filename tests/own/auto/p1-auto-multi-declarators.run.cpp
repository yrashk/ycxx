// θ step 1 (#279), spec-only: several declarators in one `auto` declaration, each deducing the same type,
// including a pointer and a reference declarator ([dcl.spec.auto.general],
// [dcl.type.auto.deduct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  int v = 3;
  auto a = 1, b = 2;
  static_assert(same<decltype(a), int>::value);
  static_assert(same<decltype(b), int>::value);
  auto x = v, *p = &v, &r = v;
  static_assert(same<decltype(x), int>::value);
  static_assert(same<decltype(p), int*>::value);
  static_assert(same<decltype(r), int&>::value);
  r = 4;
  if (*p != 4) return 1;
  const auto c1 = 'a', c2 = 'b';
  static_assert(same<decltype(c2), const char>::value);
  if (a + b != 3) return 2;
  if (x != 3) return 3;
  if (c2 - c1 != 1) return 4;
  return 0;
}
