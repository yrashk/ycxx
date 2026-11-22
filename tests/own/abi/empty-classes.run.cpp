// #116: as Clang on arm64 macOS, an empty class passes and returns nothing, in both directions.
// XFAIL: gcc@darwin-arm64 GCC on Darwin passes empty classes, which Clang ignores; on x86-64 Linux they agree
// PEER: empty-classes-peer.cpp
// EXIT: 0
struct E {};
int peer_e(int a, E e, int b);
E peer_make_e(int);
int peer_calls_e();
int main_e(int a, E e, int b) { return a * 10 + b; }
int main() {
  E e;
  if (peer_e(1, e, 2) != 12) return 1;
  E f = peer_make_e(3);
  if (peer_calls_e() != 12) return 2;
  (void)f;
  return 0;
}
