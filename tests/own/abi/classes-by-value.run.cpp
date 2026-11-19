// #116: classes by value across objects of ycxx and Clang (AAPCS64 §6.8.2 C.10–C.12, §6.9;
// Itanium C++ ABI §3.1.2): trivially copyable classes of 3, 8, 12 and 16 bytes in x0/x1 both
// ways, a 24-byte one through x8, a class with a non-trivial copy constructor and destructor by
// the address of a temporary, and Clang calling back into ycxx's functions.
// PEER: classes-by-value-peer.cpp
// EXIT: 0
struct S8 { int a, b; };
struct S12 { int a, b, c; };
struct S16 { long a, b; };
struct S24 { long a, b, c; };
struct S3 { char a, b, c; };
struct NT { int v; NT(int x) : v(x) {} NT(const NT& o) : v(o.v + 100) {} ~NT() {} };
S8 peer_s8(S8 x, int k);
S12 peer_s12(S12 x);
S16 peer_s16(S16 x, S16 y);
S24 peer_s24(S24 x);
S3 peer_s3(S3 x);
int peer_nt(NT n);
NT peer_make_nt(int v);
int peer_calls_back();
S12 main_s12(S12 x) { return {x.a * 3, x.b * 3, x.c * 3}; }
S24 main_s24(S24 x, int k) { return {x.a + k, x.b + k, x.c + k}; }
int main() {
  S8 a = peer_s8({1, 2}, 5);
  if (a.a != 6 || a.b != 7) return 1;
  S12 b = peer_s12({1, 2, 3});
  if (b.a != 3 || b.b != 2 || b.c != 1) return 2;
  S16 c = peer_s16({1, 2}, {10, 20});
  if (c.a != 21 || c.b != 12) return 3;
  S24 d = peer_s24({1, 2, 3});
  if (d.a != 2 || d.b != 4 || d.c != 6) return 4;
  S3 e = peer_s3({'a', 'b', 'c'});
  if (e.a != 'c' || e.c != 'a') return 5;
  NT n(7);
  if (peer_nt(n) != 107) return 6;
  NT m = peer_make_nt(9);
  if (m.v != 9) return 7;
  if (peer_calls_back() != 3 * 1 + 6 * 10 + 9 * 100 + 36) return 8;
  return 0;
}
