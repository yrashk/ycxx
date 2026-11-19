// ι (#300), from the review:  a copy constructor whose other parameters have defaults is the copy constructor
// ([class.copy.ctor]/1-2): it copies by itself and as a member's copy.
// EXIT: 0
int copies = 0;
struct S { int v; S(int v) : v(v) {} S(const S& o, int extra = 100) : v(o.v + extra) { ++copies; } };
struct W { S s; };
int take(S s) { return s.v; }
int main() {
  S a(1); S b = a; S c(a); int t = take(a);
  if (b.v != 101 || c.v != 101 || t != 101 || copies != 3) return 1;
  W w1{S(1)}; W w2 = w1;
  return w2.s.v == 101 && copies == 4 ? 0 : 2;
}
