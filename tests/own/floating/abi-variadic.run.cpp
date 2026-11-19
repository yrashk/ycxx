// [expr.call]/12, [conv.fpprom], [cstdarg.syn]: variadic callees in the peer read with va_arg
// what the test passes through an ellipsis: float arguments arrive promoted to double, and more
// than eight floating arguments, interleaved with integers (Apple arm64 puts every argument
// matched by ... on the stack).
// PEER: abi-variadic-peer.cpp
// EXIT: 0
double peer_vsum(int n, ...);
double peer_vmixed(const char* kinds, ...);

int main() {
  float f = 0.25f;
  double s = peer_vsum(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.5);
  if (s != 55.5) return 1;
  if (peer_vsum(3, f, f, 1.5f) != 2.0) return 2;
  if (peer_vsum(0) != 0) return 3;
  // i: int, d: double, l: long long, L: long double; weighted by position.
  double m = peer_vmixed("idldLdi", 1, 0.5, 2LL, f, 0.125L, 3.0, 4);
  if (m != 1 + 2 * 0.5 + 3 * 2 + 4 * 0.25 + 5 * 0.125 + 6 * 3.0 + 7 * 4) return 4;
  return 0;
}
