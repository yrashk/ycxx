// θ step 1 (#279), spec-only: `decltype(auto)` variables: an unparenthesized id-expression gives its
// declared type, a parenthesized one an lvalue reference, a call returning a reference keeps it
// ([dcl.type.auto.deduct], [dcl.type.decltype]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int g = 1;
int& ref() { return g; }
int val() { return 2; }

int main() {
  int x = 5;
  const int cx = 6;
  decltype(auto) a = x;
  static_assert(same<decltype(a), int>::value);
  decltype(auto) b = (x);
  static_assert(same<decltype(b), int&>::value);
  b = 7;
  if (x != 7) return 1;
  decltype(auto) c = cx;
  static_assert(same<decltype(c), const int>::value);
  decltype(auto) d = ref();
  static_assert(same<decltype(d), int&>::value);
  d = 8;
  if (g != 8) return 2;
  decltype(auto) e = val();
  static_assert(same<decltype(e), int>::value);
  decltype(auto) h = x + 1;
  static_assert(same<decltype(h), int>::value);
  if (e != 2 || h != 8 || a != 5 || c != 6) return 3;
  return 0;
}
