// θ step 1 (#279), spec-only (split from p1-decltype-auto-var): `decltype(auto)` from a call
// returning an rvalue reference keeps it ([dcl.type.auto.deduct], [dcl.type.decltype]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int g = 1;
int&& xval() { return static_cast<int&&>(g); }

int main() {
  decltype(auto) f = xval();
  static_assert(same<decltype(f), int&&>::value);
  f = 3;
  return g == 3 ? 0 : 1;
}
