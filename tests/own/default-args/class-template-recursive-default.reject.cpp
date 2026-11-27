// A default argument that takes itself is diagnosed ([dcl.fct.default], [class.mem.general]),
// once (#356).
template<class T> struct S {
  static int f(int x = f()) { return x; } //! error
};
int main() { return S<int>::f(); }
