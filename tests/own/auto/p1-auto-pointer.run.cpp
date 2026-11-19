// θ step 1 (#279), spec-only: `auto*` and `const auto*` deduce the pointee type from a pointer initializer
// ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

int main() {
  int x = 5;
  auto* p = &x;
  static_assert(same<decltype(p), int*>::value);
  *p = 6;
  if (x != 6) return 1;
  const auto* cp = &x;
  static_assert(same<decltype(cp), const int*>::value);
  if (*cp != 6) return 2;
  const char k = 'k';
  auto* pk = &k;
  static_assert(same<decltype(pk), const char*>::value);
  if (*pk != 'k') return 3;
  auto q = &x;
  static_assert(same<decltype(q), int*>::value);
  int* const cpx = &x;
  auto q2 = cpx;
  static_assert(same<decltype(q2), int*>::value);
  auto* const pc = &x;
  static_assert(same<decltype(pc), int* const>::value);
  if (q != q2 || pc != p) return 4;
  return 0;
}
