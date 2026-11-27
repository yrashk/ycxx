// A default argument of a member function template of an ordinary class is a complete-class
// context ([class.mem.general], [temp.mem], [dcl.fct.default]): it names members declared
// later in the class.
// XFAIL: ycxx an ordinary class's member template defaults are analysed at their declaration (#1607)
// EXIT: 0
struct Plain {
  int v;
  template<class U> Plain(U, int x = sizeof(U) + bias) : v(x) {}
  template<class U> U g(U u, U d = U(later)) { return u + d; }

  static constexpr int bias = 30;
  static constexpr int later = 5;
};

int main() {
  Plain p('c');
  if (p.v != 31) return 1;
  if (p.g(1) != 6 || p.g<long>(10) != 15L) return 2;
  return 0;
}
