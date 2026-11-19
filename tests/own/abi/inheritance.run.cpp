// #308: classes with bases across objects of ycxx and Clang: the same layout (bases first, a
// derived member in a non-POD base's tail padding), pointers converted to bases on either side,
// a base constructor and destructor defined by Clang run for a ycxx derived object, and a Clang
// object's base members read by ycxx. By value: a class whose data is all in its base (not an
// empty class), and a homogeneous floating-point aggregate made of a base's member and its own.
// PEER: inheritance-peer.cpp
// EXIT: 0
struct A { long a; };
struct B { char b; int bb; };
struct D : A, B { char d; };
struct U { int i; char c; U(int v); ~U(); };
struct DU : U { char d; DU() : U(4), d(5) {} };
long peer_read(const D& d);
int peer_b(B* b);
D peer_make();
int peer_du(const DU& x);
int peer_dtors();
struct OnlyBase : A {};
struct FB { float f; };
struct FD : FB { float g; };
int peer_only(int x, OnlyBase o, int y);
float peer_hfa(FD d);
FD peer_make_hfa();
int main() {
  D d;
  d.a = 1;
  d.b = 2;
  d.bb = 3;
  d.d = 4;
  if (peer_read(d) != 1234) return 1;
  if (peer_b(&d) != 23) return 2;
  D m = peer_make();
  B& rb = m;
  if (m.a != 5 || rb.b != 6 || m.bb != 7 || m.d != 8) return 3;
  {
    DU x;
    if (sizeof(DU) != 8 || peer_du(x) != 45) return 4;
  }
  if (peer_dtors() != 1) return 5;
  OnlyBase ob;
  ob.a = 2;
  if (peer_only(1, ob, 3) != 123) return 6;
  FD fd;
  fd.f = 1;
  fd.g = 2;
  if (peer_hfa(fd) != 12.0f) return 7;
  FD mf = peer_make_hfa();
  if (mf.f != 3 || mf.g != 4) return 8;
  return 0;
}
