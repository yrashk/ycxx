// [expr.call], [dcl.fct], [basic.fundamental]: float, double and long double parameters and
// results across objects, in both directions: the test calls the peer (compiled by Clang) and the
// peer calls the test's functions.
// PEER: abi-scalars-peer.cpp
// EXIT: 0
float peer_f(float x);
double peer_d(double x);
long double peer_ld(long double x);
double peer_mix(float a, double b, long double c, float d);
float peer_ret_float(double x);
int peer_calls_main();

float main_f(float x) { return x * 2 + 0.5f; }
double main_d(double x) { return x / 4; }
long double main_ld(long double x) { return x - 1; }
double main_mix(float a, double b, long double c, float d) { return a + b * 10 + c * 100 + d * 1000; }
float main_ret_float(double x) { return x; }

int main() {
  if (peer_f(1.25f) != 3.0f) return 1;
  if (peer_d(3.0) != 0.75) return 2;
  if (peer_ld(0.5L) != -0.5L) return 3;
  if (peer_mix(1, 2, 3, 4) != 4321.0) return 4;
  if (peer_ret_float(0.1) != 0.1f) return 5;
  int r = peer_calls_main();
  if (r != 0) return 10 + r;
  return 0;
}
