// #116, from the review: as Clang on arm64 macOS, an empty class passes and returns nothing. A
// class with a user-provided move constructor, or a member that has one, is not trivially
// copyable for calls ([class.temporary]/3) and goes by address, as in Clang and GCC (Apple
// Clang 21 passes it in registers). GCC passes empty classes.
// XFAIL: gcc GCC passes empty classes, which Clang ignores
// PEER: empty-and-move-only-peer.cpp
// EXIT: 0
struct E {};
struct MV2 { int v; MV2(int x) : v(x) {} MV2(MV2&& o) : v(o.v) {} };
struct HasMV2 { MV2 m; int k; };
int peer_e(int a, E e, int b);
E peer_make_e(int);
int peer_mv2(MV2 m, int k);
MV2 peer_r_mv2();
int peer_has(HasMV2 h);
int peer_calls_e();
int main_e(int a, E e, int b) { return a * 10 + b; }
int main() {
  E e;
  if (peer_e(1, e, 2) != 12) return 1;
  E f = peer_make_e(3);
  if (peer_calls_e() != 12) return 2;
  if (peer_mv2(MV2(3), 1) != 31) return 3;
  MV2 r = peer_r_mv2();
  if (r.v != 5) return 4;
  if (peer_has(HasMV2{MV2(7), 2}) != 72) return 5;
  (void)f;
  return 0;
}
