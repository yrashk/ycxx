// The other side of abi-structs.run.cpp, compiled by Clang.
struct D5 { double a, b, c, d, e; };
struct DI { double d; int i; };
struct DF { double d; float f; };
struct FI { float f; int i; };
struct FF { float a, b; };
struct Arr { double d[3]; };
struct P2 { double x, y; };
struct Nested { P2 p; double z; };

double peer_d5(D5 s, double k) { return (s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * s.e) * k; }
D5 peer_make_d5(double base) { return {base, base + 0.5, base + 1, base + 1.5, base + 2}; }
double peer_mixed(DI a, DF b, FI c, FF d) {
  return a.d + a.i + 10 * (b.d + b.f) + 100 * (c.f + c.i) + 1000 * (d.a + d.b);
}
DI peer_make_di(double d, int i) { return {d, i}; }
DF peer_make_df(double d, float f) { return {d, f}; }
FI peer_make_fi(float f, int i) { return {f, i}; }
FF peer_make_ff(float a, float b) { return {a, b}; }
Arr peer_scale_arr(Arr a, double k) { return {{a.d[0] * k, a.d[1] * k, a.d[2] * k}}; }
Nested peer_shift(Nested n, double k) { return {{n.p.x + k, n.p.y + k}, n.z + k}; }

double main_d5(D5 s, double k);
D5 main_make_d5(double base);
double main_mixed(DI a, DF b, FI c, FF d);
DF main_make_df(double d, float f);
FI main_make_fi(float f, int i);
Nested main_shift(Nested n, double k);

int peer_calls_main() {
  if (main_d5({0, 0, 0, 0, 1}, 3) != 15) return 1;
  D5 m = main_make_d5(-2);
  if (m.a != -2 || m.b != -1 || m.e != 2) return 2;
  if (main_mixed({1, 1}, {1, 1}, {1, 1}, {1, 1}) != 2222) return 3;
  DF df = main_make_df(1e300, 0.5f);
  if (df.d != 1e300 || df.f != 0.5f) return 4;
  FI fi = main_make_fi(-0.5f, -1);
  if (fi.f != -0.5f || fi.i != -1) return 5;
  Nested n = main_shift({{0, -1}, -2}, 1);
  if (n.p.x != 1 || n.p.y != 0 || n.z != -1) return 6;
  return 0;
}
