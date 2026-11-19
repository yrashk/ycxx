// The other side of sizeof-class.run.cpp, compiled by Clang.
struct P { char c; int i; char d; };
struct N { P p; char c; long l; };
union U { char c; long l; int i[3]; };
unsigned long peer_size_p() { return sizeof(P); }
unsigned long peer_size_n() { return sizeof(N); }
unsigned long peer_size_u() { return sizeof(U); }
long peer_read(N* n) { return n->p.d * 100 + n->c * 10 + n->l; }
