// #340: a static data member template of a class template whose initializer depends on both the
// class template's parameter and its own, named unqualified inside the class and as
// `C<A>::v<B>` from outside ([temp.mem], [temp.pre]/2, [class.static.data], [temp.inst]/7).
// EXIT: 0
template <class A> struct C {
  template <class B> static constexpr int v = int(sizeof(A)) * 10 + int(sizeof(B));
  static constexpr int own = v<char>;
  static int body() { return v<short>; }
  static int defaulted(int x = v<A>) { return x; }
  template <class B> static int pick() {
    if constexpr (v<B> == v<A>) return 1;
    else return 2;
  }
};
static_assert(C<int>::v<char> == 41);
static_assert(C<char>::v<int> == 14);
static_assert(C<long long>::own == 81);
static_assert(C<int>::template v<short> == 42);
int main() {
  int r = C<int>::body() + C<char>::defaulted() + C<int>::pick<unsigned>()
          + C<int>::pick<char>();
  // 42 + 11 + 1 + 2
  return r == 56 ? 0 : 1;
}
