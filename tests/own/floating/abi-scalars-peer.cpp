// The other side of abi-scalars.run.cpp, compiled by Clang.
float peer_f(float x) { return x * 2 + 0.5f; }
double peer_d(double x) { return x / 4; }
long double peer_ld(long double x) { return x - 1; }
double peer_mix(float a, double b, long double c, float d) { return a + b * 10 + c * 100 + d * 1000; }
float peer_ret_float(double x) { return x; }

float main_f(float x);
double main_d(double x);
long double main_ld(long double x);
double main_mix(float a, double b, long double c, float d);
float main_ret_float(double x);

int peer_calls_main() {
  if (main_f(1.25f) != 3.0f) return 1;
  if (main_d(3.0) != 0.75) return 2;
  if (main_ld(0.5L) != -0.5L) return 3;
  if (main_mix(4, 3, 2, 1) != 1234.0) return 4;
  if (main_ret_float(0.1) != 0.1f) return 5;
  return 0;
}
