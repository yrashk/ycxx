// θ step 1 (#279): placeholders in templates. A type-dependent initializer or return operand
// deduces nothing in the definition; each instantiation deduces from its own
// ([dcl.spec.auto.general]/12, [temp.inst]); a variable template's and a class template member's
// placeholders likewise. A trailing return type sees the parameters ([basic.scope.param]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

template<class T> T sum3(T a) {
  auto b = a + a;
  const auto& c = b;
  auto* p = &b;
  return c + *p - a;
}
template<class T> constexpr auto vt = T(3) + 1;
template<class T> struct Box {
  T v;
  auto get() const { return v; }
  auto& ref() { return v; }
};
template<class T> auto pick(bool f, T a, T b) {
  if (f) return a;
  return b;
}
auto plus1(int x) -> decltype(x + 1) { return x + 1; }
template<class T> auto fact(T n) {
  if (n <= 1) return T(1);
  return n * fact(n - 1);
}

static_assert(same<decltype(vt<long>), const long>::value);
static_assert(vt<char> == 4);
static_assert(same<decltype(plus1(1)), int>::value);

int main() {
  if (sum3(5) != 15 || sum3(7L) != 21L) return 1;
  Box<char> b{'a'};
  static_assert(same<decltype(b.get()), char>::value);
  b.ref() = 'z';
  if (b.get() != 'z') return 2;
  if (pick(false, 1, 2) != 2) return 3;
  static_assert(same<decltype(pick(true, 1L, 2L)), long>::value);
  if (plus1(4) != 5) return 4;
  if (fact(5) != 120 || fact(4L) != 24L) return 5;
  return 0;
}
