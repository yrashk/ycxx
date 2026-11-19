// [expr.call], [dcl.fct]: floating aggregates that no longer fit in the remaining floating
// registers: three structs of three doubles need nine of the eight v registers, so (AAPCS64
// C.3) the third goes whole on the stack and no later floating argument uses a register; across
// objects in both directions.
// PEER: abi-hfa-exhaust-peer.cpp
// EXIT: 0
struct D3 { double a, b, c; };
struct F4 { float a, b, c, d; };

double peer_three(D3 x, D3 y, D3 z, double after);
double peer_seven_then(double a, double b, double c, double d, double e, double f, double g,
                       F4 w, float after);
int peer_calls_main();

double main_three(D3 x, D3 y, D3 z, double after) {
  return (x.a + x.b + x.c) + 10 * (y.a + y.b + y.c) + 100 * (z.a + z.b + z.c) + 1000 * after;
}
double main_seven_then(double a, double b, double c, double d, double e, double f, double g,
                       F4 w, float after) {
  return a + b + c + d + e + f + g + 10 * (w.a + w.b + w.c + w.d) + 100 * after;
}

int main() {
  if (peer_three({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1) != 1111) return 1;
  if (peer_three({0.5, 0.5, 0}, {1, 1, 1}, {2, 0, 2}, 0.25) != 681) return 2;
  if (peer_seven_then(1, 1, 1, 1, 1, 1, 1, {1, 2, 3, 4}, 0.5f) != 157) return 3;
  int r = peer_calls_main();
  return r == 0 ? 0 : 10 + r;
}
