// θ step 1 (#279), spec-only: `const auto& [a, b]` binds to the object as const: the names refer to its
// members and decltype gives the const-qualified member type ([dcl.struct.bind],
// [dcl.type.decltype]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct S {
  int a;
  long b;
};

int main() {
  S s{1, 2L};
  const auto& [a, b] = s;
  static_assert(same<decltype(a), const int>::value);
  static_assert(same<decltype(b), const long>::value);
  s.a = 11;
  if (a != 11) return 1;
  if (&b != &s.b) return 2;
  int arr[2] = {4, 5};
  const auto& [e, f] = arr;
  static_assert(same<decltype(e), const int>::value);
  arr[1] = 50;
  if (f != 50) return 3;
  return 0;
}
