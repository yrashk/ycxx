// #194: ycxx's sizeof agrees with the layout the peer's compiler uses for the same classes, and
// with ycxx's own back end: the peer reads members ycxx wrote, at the offsets it computes.
// PEER: sizeof-class-peer.cpp
// EXIT: 0
struct P { char c; int i; char d; };
struct N { P p; char c; long l; };
union U { char c; long l; int i[3]; };
unsigned long peer_size_p();
unsigned long peer_size_n();
unsigned long peer_size_u();
long peer_read(N* n);
int main() {
  if (sizeof(P) != peer_size_p()) return 1;
  if (sizeof(N) != peer_size_n()) return 2;
  if (sizeof(U) != peer_size_u()) return 3;
  N n;
  n.p.d = 3;
  n.c = 4;
  n.l = 5;
  if (peer_read(&n) != 345) return 4;
  return 0;
}
