// The other side of abi-hfa.run.cpp, compiled by Clang.
struct F1 { float a; };
struct F2 { float a, b; };
struct F3 { float a, b, c; };
struct F4 { float a, b, c, d; };
struct D1 { double a; };
struct D2 { double a, b; };
struct D3 { double a, b, c; };
struct D4 { double a, b, c, d; };

float peer_f(F1 x, F2 y, F3 z, F4 w) {
  return x.a + 2 * (y.a + y.b) + 3 * (z.a + z.b + z.c) + 4 * (w.a + w.b + w.c + w.d);
}
double peer_d(D1 x, D2 y, D3 z) { return x.a + 2 * (y.a + y.b) + 3 * (z.a + z.b + z.c); }
double peer_d4(D4 w) { return w.a + 2 * w.b + 3 * w.c + 4 * w.d; }
F2 peer_make_f2(float a, float b) { return {a, b}; }
F3 peer_make_f3(float base) { return {base, base * 2, base * 3}; }
F4 peer_make_f4(float base) { return {base, base + 1, base + 2, base + 3}; }
D2 peer_make_d2(double a, double b) { return {a, b}; }
D4 peer_make_d4(double base) { return {base, base * 2, base * 3, base * 4}; }
D1 peer_make_d1(double a) { return {a}; }

float main_f(F1 x, F2 y, F3 z, F4 w);
double main_d(D1 x, D2 y, D3 z);
double main_d4(D4 w);
F3 main_make_f3(float base);
D4 main_make_d4(double base);
D2 main_swap(D2 x);

int peer_calls_main() {
  if (main_f({2}, {1, 0}, {0, 1, 0}, {0, 0, 0, 0.5f}) != 9) return 1;
  if (main_d({1}, {2, 3}, {4, 5, 6}) != 56) return 2;
  if (main_d4({4, 3, 2, 1}) != 20) return 3;
  F3 f3 = main_make_f3(1.5f);
  if (f3.a != 1.5f || f3.b != 3.0f || f3.c != 4.5f) return 4;
  D4 d4 = main_make_d4(0.5);
  if (d4.a != 0.5 || d4.b != 1.5 || d4.c != 2.5 || d4.d != 3.5) return 5;
  D2 s = main_swap({0.25, -8});
  if (s.a != -8 || s.b != 0.25) return 6;
  return 0;
}
