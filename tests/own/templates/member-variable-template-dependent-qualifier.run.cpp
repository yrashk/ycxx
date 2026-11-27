// #340: a static data member template named through a dependent qualifier as
// `T::template v<args>` ([temp.names]/4, [temp.res], [temp.mem]).
// EXIT: 0
struct One {
  template <class U> static constexpr int v = int(sizeof(U));
};
template <class A> struct Two {
  template <class U> static constexpr int v = int(sizeof(A)) + 100 * int(sizeof(U));
};
template <class T> int read() { return T::template v<int>; }
template <class T> constexpr int twice = T::template v<char> * 2;
template <class T> struct User {
  static constexpr int value = T::template v<short>;
};
static_assert(twice<One> == 2);
static_assert(User<One>::value == 2);
static_assert(User<Two<char>>::value == 201);
int main() {
  int r = read<One>() + read<Two<short>>();
  // 4 + 402
  return r == 406 ? 0 : 1;
}
