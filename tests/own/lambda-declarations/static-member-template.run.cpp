// A lambda in the initializer of a class template's static data member (#338): each
// specialization, instantiated when the member is used, has its own closure type ([temp.inst]/3).
template <class T>
struct A {
  static constexpr T m = [] { T r = 0; return r; }();
};
template <class T>
struct Limits {  // the form of libycxx's numeric_limits
  static constexpr int digits = sizeof(T) * 8 - 1;
  static constexpr T max = [] {
    T r = 0;
    for (int i = 0; i < digits; ++i) r = static_cast<T>(r * 2 + 1);
    return r;
  }();
  static constexpr T min = -max - 1;
};
template <class T>
struct F {
  static constexpr auto f = [](T x) { return x + 1; };
  static constexpr T g = f(T(3));
  static inline T h = [] { return T(7); }();
};
template <class T>
struct P;
template <class T>
struct P<T*> {
  static constexpr int m = [] { return int(sizeof(T)); }();
  static constexpr auto g = [](int x) { return x + m; };
};
template <>
struct P<long> {
  static constexpr int m = [] { return 77; }();
};
// Never used: its body is not instantiated.
template <class T>
struct Lazy {
  static constexpr int bad = [] { return T::nope; }();
  static constexpr int good = 1;
};
static_assert(A<int>::m == 0 && Limits<int>::max == 2147483647 && Limits<signed char>::max == 127);
static_assert(Limits<signed char>::min == -128 && F<int>::f(2) == 3 && F<int>::g == 4);
static_assert(P<int*>::m == 4 && P<char*>::g(1) == 2 && P<long>::m == 77 && Lazy<int>::good == 1);
int main() {
  long one = 1;
  if (A<int>::m != 0 || F<long>::f(one) != 2 || F<short>::h != 7 || F<int>::g != 4) return 1;
  return P<char*>::g(int(one)) == 2 ? 0 : 2;
}
