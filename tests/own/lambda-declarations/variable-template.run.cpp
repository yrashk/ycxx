// A lambda in a variable template's initializer (#338): each specialization has its own closure
// type, and a fold in the body expands the template's pack ([temp.inst]/7).
template <int... V>
inline constexpr int zero = [] { int r = 0; return r; }();
template <int... V>
inline constexpr int max = [] {
  int r = 0;
  ((r = V > r ? V : r), ...);
  return r;
}();
template <class T>
constexpr auto add1 = [](T x) { return x + 1; };
template <class T>
constexpr auto twice = [](auto x, T y) { return x * 2 + y; };
template <class T>
constexpr int one = 1;
template <class T>
constexpr int one<T*> = [] { return int(sizeof(T)); }();
template <>
constexpr int one<long> = [](int x) { return x; }(9);
template <class T>
constexpr int byte = [] {
  if constexpr (sizeof(T) == 1) return 1;
  else return 2;
}();
template <class... T>
constexpr int count = []<class... U>(U...) { return int(sizeof...(U)); }(T()...);
template <class F>
constexpr int call(F f) { return f(4); }
template <class T, class U>
constexpr bool same = false;
template <class T>
constexpr bool same<T, T> = true;
static_assert(zero<1> == 0 && max<1, 7, 3> == 7 && max<> == 0);
static_assert(add1<int>(3) == 4 && twice<int>(2, 1) == 5 && call(add1<int>) == 5);
static_assert(one<int> == 1 && one<int*> == 4 && one<long> == 9 && byte<char> == 1 && byte<int> == 2);
static_assert(count<int, char, long> == 3);
static_assert(!same<decltype(add1<int>), decltype(add1<long>)>);
static_assert(same<decltype(add1<int>), decltype(add1<int>)>);
int main() {
  int z = 1;
  return add1<int>(z) == 2 && twice<long>(z, 3) == 5 && call(add1<short>) == 5 && zero<1> == 0 ? 0 : 1;
}
