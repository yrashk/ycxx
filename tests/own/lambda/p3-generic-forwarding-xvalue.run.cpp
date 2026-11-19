// θ step 3 (#281), spec-only (split from p3-generic-forwarding): an xvalue argument makes an
// `auto&&` parameter an rvalue reference ([temp.deduct.call], [dcl.ref]).
// XFAIL: ycxx static_cast to an rvalue reference of an lvalue, #199
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto kind = [](auto&& x) { return same<decltype(x), int&&>::value ? 2 : 0; };
  int i = 0;
  return kind(static_cast<int&&>(i)) == 2 ? 0 : 1;
}
