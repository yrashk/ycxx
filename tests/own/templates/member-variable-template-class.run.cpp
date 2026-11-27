// #340: a static data member template of a non-template class, named unqualified inside the
// class (a later static member's initializer, member function bodies, default arguments,
// `if constexpr`) and as `C::v<args>` from outside ([temp.mem], [temp.pre]/2,
// [class.static.data], [temp.inst]/7).
// EXIT: 0
struct C {
  template <class T> static constexpr T v = T(3);
  static constexpr int sum = v<int> + int(v<long>);
  static int body() { return v<int> * 2; }
  static int defaulted(int x = v<short>) { return x; }
  template <class T> static int pick() {
    if constexpr (v<T> == 3) return 1;
    else return 2;
  }
  int member() const { return v<int> + 1; }
};
static_assert(C::v<int> == 3);
static_assert(C::v<double> == 3.0);
static_assert(C::sum == 6);
int main() {
  C c;
  int r = C::v<int> + C::body() + C::defaulted() + C::defaulted(10) + C::pick<int>()
          + c.member();
  // 3 + 6 + 3 + 10 + 1 + 4
  return r == 27 ? 0 : 1;
}
