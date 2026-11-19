// [expr.call]/12, [cstdarg.syn]: floating aggregates and long doubles passed through an ellipsis
// to a variadic peer that reads them back with va_arg (on Apple arm64 every argument matched by
// ... goes on the stack, so a float aggregate is not passed in v registers there).
// PEER: abi-variadic-structs-peer.cpp
// EXIT: 0
struct D2 { double a, b; };
struct F3 { float a, b, c; };
struct D5 { double a, b, c, d, e; };
struct DI { double d; int i; };

double peer_vstructs(int n, ...);

int main() {
  D2 d2{1.5, 2.5};
  F3 f3{0.25f, 0.5f, 0.75f};
  D5 d5{1, 2, 3, 4, 5};
  DI di{0.125, 8};
  long double ld = 0.5L;
  double r = peer_vstructs(5, d2, f3, d5, di, ld);
  return r == 4 + 2 * 1.5 + 3 * 15 + 4 * 8.125 + 5 * 0.5 ? 0 : 1;
}
