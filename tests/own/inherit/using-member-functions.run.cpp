// Member using-declarations naming base member functions ([namespace.udecl]): they join the
// derived class's overload set (/1, [over.match.funcs.general]/4), a derived member with the
// same parameter-type-list and qualifiers hides the base's (/11), the call's object is the base
// subobject, the using-declaration's access governs (/16), and a dependent base is named in
// each specialization ([temp.dep.type]) (#1150).
struct A { int a = 100; };
struct F {
  int k = 7;
  int operator()(int x) const { return x + k; }
  int g(int) { return 1; }
  int g(long) { return 2; }
  static int s(int) { return 30; }
  template<class T> int t(T) { return 40; }
};
struct S : A, F {   // F is not at offset 0: the call adjusts the object
  using F::operator();
  using F::g;
  using F::s;
  using F::t;
  int g(int) { return 3; }              // hides F::g(int)
  int operator()() const { return 5; }  // overloads F::operator()
  int h() { return g(1) * 10 + g(1L) + s(0) + t(1); }
};
struct G : S {};  // found through a base that introduced it
template<class Fn> struct D : Fn {
  using Fn::operator();
  int operator()(char) const { return 9; }
};
template<class Fn, int K> struct P;
template<class Fn> struct P<Fn, 0> : Fn {
  using Fn::operator();
  int operator()() const { return 0; }
};
template<class T> struct N : F { using F::g; int g(int) { return 4; } };
class Priv : F { public: using F::g; };
struct V { virtual int v(int) { return 1; } };
struct W : V { int v(int) override { return 2; } };
struct X : W { using V::v; int v(long) { return 3; } };  // dispatches to the final overrider
int main() {
  S s;
  G g;
  D<F> d;
  Priv p;
  X x;
  V& vr = x;
  int r = 0;
  if (s(1) != 8 || s() != 5) r |= 1;
  if (s.g(1) != 3 || s.g(1L) != 2) r |= 2;
  if (s.h() != 102) r |= 4;
  if (g(2) != 9 || g.g(1L) != 2 || g.g(1) != 3) r |= 8;
  if (d(3) != 10 || d('c') != 9 || D<F>{}(1) != 8) r |= 16;
  if (P<F, 0>{}(5) != 12 || P<F, 0>{}() != 0) r |= 32;
  if (N<int>{}.g(1) != 4 || N<int>{}.g(1L) != 2) r |= 64;
  if (p.g(1L) != 2) r |= 128;
  if (x.v(1) != 2 || x.v(1L) != 3 || vr.v(0) != 2) r |= 256;
  if (S::s(1) != 30) r |= 512;
  return r;
}
