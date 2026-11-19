// The other side of inheritance.run.cpp, compiled by Clang.
struct A { long a; };
struct B { char b; int bb; };
struct D : A, B { char d; };
struct U { int i; char c; U(int v); ~U(); };
struct DU : U { char d; DU() : U(4), d(5) {} };
static int dtors = 0;
U::U(int v) : i(v), c(0) {}
U::~U() { ++dtors; }
long peer_read(const D& d) { return d.a * 1000 + d.b * 100 + d.bb * 10 + d.d; }
int peer_b(B* b) { return b->b * 10 + b->bb; }
D peer_make() {
  D d;
  d.a = 5;
  d.b = 6;
  d.bb = 7;
  d.d = 8;
  return d;
}
int peer_du(const DU& x) { return x.i * 10 + x.d; }
int peer_dtors() { return dtors; }
struct OnlyBase : A {};
struct FB { float f; };
struct FD : FB { float g; };
int peer_only(int x, OnlyBase o, int y) { return int(x * 100 + o.a * 10 + y); }
float peer_hfa(FD d) { return d.f * 10 + d.g; }
FD peer_make_hfa() {
  FD d;
  d.f = 3;
  d.g = 4;
  return d;
}
