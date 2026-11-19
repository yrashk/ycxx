// ι (#300): non-dependent default arguments of member function and constructor templates ([dcl.fct.default],
// [temp.mem], [temp.deduct.call]/1).
// EXIT: 0
struct S {
  int base = 10;
  template <class T> int f(T x, int n = 2) { return base + x + n; }
  template <class T> static int g(T x, int n = 3) { return x * n; }
};
struct P {
  int v;
  template <class T> P(T x, int k = 4) : v(x + k) {}
};
int main() {
  S s;
  if (s.f(1) != 13) return 1;
  if (s.f(1, 5) != 16) return 2;
  if (S::g(2) != 6) return 3;
  P p(1);
  if (p.v != 5) return 4;
  P q = 2;  // a converting constructor template, the rest defaulted
  if (q.v != 6) return 5;
  return 0;
}
