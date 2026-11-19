// θ step 1 (#279), spec-only: `auto [a, b] = arr;` copies the array and binds each name to an element of
// the copy; the names have the element type ([dcl.struct.bind], [dcl.type.decltype]).
// XFAIL: ycxx structured bindings, #285
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  int arr[2] = {3, 4};
  auto [a, b] = arr;
  static_assert(same<decltype(a), int>::value);
  if (a != 3 || b != 4) return 1;
  a = 30;
  if (arr[0] != 3) return 2;
  char cs[3] = {'x', 'y', 'z'};
  auto [p, q, r] = cs;
  static_assert(same<decltype(r), char>::value);
  if (p != 'x' || q != 'y' || r != 'z') return 3;
  return 0;
}
