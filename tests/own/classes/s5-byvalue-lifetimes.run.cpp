// #116: a parameter of class type is an object the caller initializes from the argument and
// destroys at the end of the full-expression ([expr.call]/7; Itanium C++ ABI §3.1.2.3); a class
// result initializes the caller's object directly ([stmt.return], [class.copy.elision]/1:
// returning a prvalue copies nothing); a discarded result is a temporary ([expr.context]/2).
// EXIT: 0
int constructed = 0, copied = 0, destroyed = 0;
struct T {
  int v;
  T(int x) : v(x) { ++constructed; }
  T(const T& o) : v(o.v) { ++copied; }
  ~T() { ++destroyed; }
};
int take(T t) { return t.v; }
T make(int v) { return T(v); }
T pass(T t) { return T(t.v + 1); }
int main() {
  T a(1);
  int r = take(a);  // copied in, destroyed at the end of this full-expression
  if (r != 1 || copied != 1 || destroyed != 1) return 1;
  r = take(T(2));   // a prvalue argument initializes the parameter: no copy
  if (r != 2 || copied != 1 || destroyed != 2) return 2;
  T b = make(3);
  if (b.v != 3 || copied != 1 || destroyed != 2) return 3;          // no copy, no temporary
  make(4);
  if (destroyed != 3) return 4;                                      // the discarded result
  T c = pass(b);
  if (c.v != 4 || copied != 2 || destroyed != 4) return 5;           // b copied in, destroyed
  return constructed == 5 ? 0 : 6;
}
