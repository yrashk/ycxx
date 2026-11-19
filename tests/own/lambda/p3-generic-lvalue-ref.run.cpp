// θ step 3 (#281), spec-only: an `auto&` parameter of a generic lambda is a reference to an invented
// template parameter; it binds to the argument lvalue (modifications are seen by the caller) and deduces
// a const-qualified type for a const lvalue ([expr.prim.lambda.closure], [dcl.fct],
// [temp.deduct.call]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  auto bump = [](auto& x) { x += 1; };
  int i = 1;
  long l = 10;
  char c = 'a';
  bump(i);
  bump(l);
  bump(c);
  if (i != 2) return 1;
  if (l != 11) return 2;
  if (c != 'b') return 3;
  auto addr = [](auto& x) { return &x; };
  if (addr(i) != &i) return 4;
  const int k = 5;
  if (!same<decltype(addr(k)), const int*>::value) return 5;
  if (addr(k) != &k) return 6;
  if (!same<decltype(addr(l)), long*>::value) return 7;
  int a[3] = {1, 2, 3};
  auto first = [](auto& arr) { return arr[0] + sizeof(arr) / sizeof(arr[0]); };
  if (first(a) != 4) return 8;
  return 0;
}
