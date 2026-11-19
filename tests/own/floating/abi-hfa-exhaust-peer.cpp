// The other side of abi-hfa-exhaust.run.cpp, compiled by Clang.
struct D3 { double a, b, c; };
struct F4 { float a, b, c, d; };

double peer_three(D3 x, D3 y, D3 z, double after) {
  return (x.a + x.b + x.c) + 10 * (y.a + y.b + y.c) + 100 * (z.a + z.b + z.c) + 1000 * after;
}
double peer_seven_then(double a, double b, double c, double d, double e, double f, double g,
                       F4 w, float after) {
  return a + b + c + d + e + f + g + 10 * (w.a + w.b + w.c + w.d) + 100 * after;
}

double main_three(D3 x, D3 y, D3 z, double after);
double main_seven_then(double a, double b, double c, double d, double e, double f, double g,
                       F4 w, float after);

int peer_calls_main() {
  if (main_three({1, 2, 3}, {0, 0, 0.5}, {1, 0, 0}, 2) != 2111) return 1;
  if (main_seven_then(0, 0, 0, 0, 0, 0, 2, {0.5f, 0, 0, 0.5f}, 3.0f) != 312) return 2;
  return 0;
}
