// #340: `static constexpr auto` static data member templates take their type from each
// specialization's initializer ([dcl.spec.auto], [temp.mem], [class.static.data]).
// EXIT: 0
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };
struct C {
  template <class T> static constexpr auto v = T(2);
  template <int N> static constexpr auto half = N / 2;
};
template <class A> struct D {
  template <class T> static constexpr auto prod = A(3) * T(2);
};
static_assert(same<decltype(C::v<long>), const long>::value);
static_assert(same<decltype(C::v<char>), const char>::value);
static_assert(same<decltype(C::half<6>), const int>::value);
static_assert(D<long>::prod<int> == 6L && D<double>::prod<int> == 6.0);
static_assert(C::half<6> == 3 && D<int>::prod<int> == 6);
int main() { return C::v<int> + D<int>::prod<char> == 8 ? 0 : 1; }
