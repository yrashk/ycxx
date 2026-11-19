// [class.mem], [expr.call], [dcl.fct]: structs of one to four floats or four doubles (AAPCS64
// homogeneous floating-point aggregates, passed and returned in v registers) by value across
// objects, in both directions.
// PEER: abi-hfa-peer.cpp
// EXIT: 0
struct F1 { float a; };
struct F2 { float a, b; };
struct F3 { float a, b, c; };
struct F4 { float a, b, c, d; };
struct D1 { double a; };
struct D2 { double a, b; };
struct D3 { double a, b, c; };
struct D4 { double a, b, c, d; };

float peer_f(F1 x, F2 y, F3 z, F4 w);
double peer_d(D1 x, D2 y, D3 z);
double peer_d4(D4 w);
F2 peer_make_f2(float a, float b);
F3 peer_make_f3(float base);
F4 peer_make_f4(float base);
D2 peer_make_d2(double a, double b);
D4 peer_make_d4(double base);
D1 peer_make_d1(double a);
int peer_calls_main();

float main_f(F1 x, F2 y, F3 z, F4 w) {
  return x.a + 2 * (y.a + y.b) + 3 * (z.a + z.b + z.c) + 4 * (w.a + w.b + w.c + w.d);
}
double main_d(D1 x, D2 y, D3 z) { return x.a + 2 * (y.a + y.b) + 3 * (z.a + z.b + z.c); }
double main_d4(D4 w) { return w.a + 2 * w.b + 3 * w.c + 4 * w.d; }
F3 main_make_f3(float base) { return {base, base * 2, base * 3}; }
D4 main_make_d4(double base) { return {base, base + 1, base + 2, base + 3}; }
D2 main_swap(D2 x) { return {x.b, x.a}; }

int main() {
  if (peer_f({1}, {1, 1}, {1, 1, 1}, {1, 1, 1, 1}) != 30) return 1;
  if (peer_d({0.5}, {0.5, 0.5}, {0.5, 0.5, 0.5}) != 7) return 2;
  if (peer_d4({1, 2, 3, 4}) != 30) return 3;
  F2 f2 = peer_make_f2(1.5f, -2.5f);
  if (f2.a != 1.5f || f2.b != -2.5f) return 4;
  F3 f3 = peer_make_f3(0.5f);
  if (f3.a != 0.5f || f3.b != 1.0f || f3.c != 1.5f) return 5;
  F4 f4 = peer_make_f4(2);
  if (f4.a != 2 || f4.b != 3 || f4.c != 4 || f4.d != 5) return 6;
  D2 d2 = peer_make_d2(0.1, 0.2);
  if (d2.a != 0.1 || d2.b != 0.2) return 7;
  D4 d4 = peer_make_d4(10);
  if (d4.a != 10 || d4.b != 20 || d4.c != 30 || d4.d != 40) return 8;
  if (peer_make_d1(-3.5).a != -3.5) return 9;
  int r = peer_calls_main();
  return r == 0 ? 0 : 10 + r;
}
