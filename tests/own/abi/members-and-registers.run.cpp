// #116: member functions returning classes (`this` in x0, the result in registers or through x8),
// and a 16-byte class after seven integer arguments, which goes whole on the stack and leaves x7
// unused (AAPCS64 C.11), in both directions.
// PEER: members-and-registers-peer.cpp
// EXIT: 0
struct S12 { int a, b, c; };
struct S24 { long a, b, c; };
struct Box { int k; S12 twice() const; S24 big(int m) const; };
struct S16 { long a, b; };
long peer_many(int a, int b, int c, int d, int e, int f, int g, S16 s);
struct S16b { long a, b; };
long main_many(int a, int b, int c, int d, int e, int f, int g, S16b s) { return a + b + c + d + e + f + g + s.a * 100 + s.b * 1000; }
long peer_calls_many();
int main() {
  if (peer_calls_many() != 28 + 300 + 4000) return 4;
  Box x{4};
  S12 t = x.twice();
  if (t.a != 8 || t.c != 8) return 1;
  S24 b = x.big(5);
  if (b.a != 4 || b.b != 5 || b.c != 9) return 2;
  if (peer_many(1, 2, 3, 4, 5, 6, 7, {1, 2}) != 28 + 100 + 2000) return 3;
  return 0;
}
