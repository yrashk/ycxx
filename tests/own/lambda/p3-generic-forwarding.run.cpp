// θ step 3 (#281), spec-only: an `auto&&` parameter of a generic lambda is a forwarding reference: an
// lvalue argument deduces an lvalue reference type and the parameter is an lvalue reference, an rvalue
// argument makes it an rvalue reference ([expr.prim.lambda.closure], [dcl.fct], [temp.deduct.call],
// [dcl.ref]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto kind = [](auto&& x) {
    if (same<decltype(x), int&>::value) return 1;
    if (same<decltype(x), int&&>::value) return 2;
    if (same<decltype(x), const int&>::value) return 3;
    if (same<decltype(x), char&>::value) return 4;
    if (same<decltype(x), char&&>::value) return 5;
    return 0;
  };
  int i = 0;
  const int k = 1;
  char c = 'c';
  if (kind(i) != 1) return 1;
  if (kind(5) != 2) return 2;
  if (kind(k) != 3) return 3;
  if (kind(c) != 4) return 4;
  if (kind('d') != 5) return 5;
  auto set = [](auto&& x) { x = 9; };
  set(i);
  if (i != 9) return 7;
  auto addr = [](auto&& x) { return &x; };
  if (addr(i) != &i) return 8;
  if (!same<decltype(addr(k)), const int*>::value) return 9;
  return 0;
}
