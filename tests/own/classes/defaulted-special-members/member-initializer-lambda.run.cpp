// #157, #309 [class.default.ctor], [expr.prim.lambda.closure]: a lambda in a default member
// initializer that a defaulted or an implicit default constructor runs.
// EXIT: 0
struct Defaulted { int x = [](int a) { return a; }(1); Defaulted() = default; };
template <class T> struct Templated { T x = [](T a) { return a; }(2); Templated() = default; };
struct Captures { int y = 4; int x = [this] { return y + 1; }(); Captures() = default; };
struct Dynamic { virtual int f() { return x; } int x = [](int a) { return a; }(6); };

int main() {
  Defaulted d;
  Templated<int> t;
  Captures c;
  Dynamic v;
  return d.x == 1 && t.x == 2 && c.x == 5 && v.f() == 6 ? 0 : 1;
}
