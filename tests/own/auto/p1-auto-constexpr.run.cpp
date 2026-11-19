// θ step 1 (#279), spec-only: `constexpr auto` variables deduce their type and are usable in constant
// expressions ([dcl.spec.auto.general], [dcl.constexpr], [expr.const]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

constexpr auto ns = 6 * 7;
static_assert(ns == 42);
static_assert(same<decltype(ns), const int>::value);

constexpr int sq(int v) { return v * v; }

int main() {
  constexpr auto a = sq(5);
  static_assert(a == 25);
  static_assert(same<decltype(a), const int>::value);
  constexpr auto c = 'A' + 1;
  static_assert(same<decltype(c), const int>::value);
  static_assert(c == 66);
  constexpr auto l = 3L << 4;
  static_assert(same<decltype(l), const long>::value);
  static_assert(l == 48);
  int arr[a];
  static_assert(sizeof(arr) == 25 * sizeof(int));
  return a - 25;
}
