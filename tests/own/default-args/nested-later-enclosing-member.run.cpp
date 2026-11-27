// A complete-class context of a nested class is also one of its enclosing class
// ([class.mem.general]): a nested class's default argument names a member the enclosing class
// declares later, in an ordinary class and in a class template ([dcl.fct.default]).
// XFAIL: ycxx a nested class's defaults are bound before the enclosing class is complete (#1602)
// EXIT: 0
struct Outer {
  struct Nested { int get(int x = later + 1) { return x; } };
  static constexpr int later = 100;
};

template<class T>
struct OuterT {
  struct Nested { int get(int x = later + int(sizeof(T))) { return x; } };
  static constexpr int later = 200;
};

int main() {
  Outer::Nested a;
  if (a.get() != 101 || a.get(3) != 3) return 1;
  OuterT<int>::Nested b;
  if (b.get() != 204) return 2;
  return 0;
}
