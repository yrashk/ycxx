// θ step 1 (#279), spec-only: function templates with deduced return types; each specialization deduces
// its own type when it is instantiated ([dcl.spec.auto.general], [dcl.type.auto.deduct],
// [temp.inst]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

template<class T> auto twice(T x) { return x + x; }
template<class T> auto& deref(T* p) { return *p; }
template<class T> decltype(auto) id(T& r) { return (r); }
template<class T> auto first(T (&a)[2]) { return a[0]; }

int main() {
  static_assert(same<decltype(twice(1)), int>::value);
  static_assert(same<decltype(twice(2L)), long>::value);
  static_assert(same<decltype(twice('a')), int>::value);
  if (twice(21) != 42) return 1;
  if (twice(3L) != 6) return 2;
  int x = 1;
  static_assert(same<decltype(deref(&x)), int&>::value);
  deref(&x) = 2;
  if (x != 2) return 3;
  static_assert(same<decltype(id(x)), int&>::value);
  id(x) = 4;
  if (x != 4) return 4;
  char cs[2] = {'m', 'n'};
  static_assert(same<decltype(first(cs)), char>::value);
  if (first(cs) != 'm') return 5;
  return 0;
}
