// θ step 1 (#279), spec-only: member functions with deduced return types, defined in the class and outside
// it; a static member with `auto` return ([dcl.spec.auto.general], [class.mfct],
// [dcl.type.auto.deduct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct C {
  int v;
  long w;
  C(int a, long b) : v(a), w(b) {}
  auto get() const { return v; }
  auto& ref() { return v; }
  decltype(auto) wref() { return (w); }
  auto twice() const;
  static auto make(int a) { return C(a, 0L); }
};

auto C::twice() const { return w * 2; }

int main() {
  C c(1, 2L);
  static_assert(same<decltype(c.get()), int>::value);
  static_assert(same<decltype(c.ref()), int&>::value);
  static_assert(same<decltype(c.wref()), long&>::value);
  static_assert(same<decltype(c.twice()), long>::value);
  static_assert(same<decltype(C::make(1)), C>::value);
  c.ref() = 5;
  if (c.get() != 5) return 1;
  c.wref() = 21;
  if (c.twice() != 42) return 2;
  if (C::make(9).get() != 9) return 3;
  return 0;
}
