// ζ f1 (#88), from the step's review: `T()` with T = void is a void prvalue, `T(x)` with a
// reference type binds it ([expr.type.conv]/2, [expr.cast]).
// EXIT: 0
template<class T> int f() { (T(), 3); return 3; }
template<class T> int g(int& x) { T r = T(x); r = 7; return x; }
int main() {
  if (f<void>() != 3) return 1;
  int x = 1;
  if (g<int&>(x) != 7) return 2;
  return 0;
}
