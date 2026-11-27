// #340: a derived class names its base's static data member template unqualified and qualified;
// a dependent base's member needs `Base<A>::template` ([class.derived], [class.member.lookup],
// [temp.dep], [temp.mem]).
// EXIT: 0
struct Base {
  template <class T> static constexpr int v = int(sizeof(T)) * 2;
};
struct Derived : Base {
  static constexpr int a = v<char>;
  static constexpr int b = Base::v<short>;
  static int body() { return v<int> + Base::v<int>; }
};
template <class A> struct TBase {
  template <class T> static constexpr int w = int(sizeof(A)) + int(sizeof(T));
};
template <class A> struct TDerived : TBase<A> {
  static constexpr int c = TBase<A>::template w<char>;
};
static_assert(Derived::a == 2 && Derived::b == 4);
static_assert(Derived::v<long long> == 16);
static_assert(TDerived<int>::c == 5);
static_assert(TDerived<int>::w<int> == 8);
int main() { return Derived::body() == 16 ? 0 : 1; }
