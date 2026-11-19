// [class.mem], [expr.call], [dcl.fct]: structs with floating members that are not small floating
// aggregates of one type: five doubles (passed and returned by address), {double; int;},
// {double; float;}, {float; int;}, and the aggregates {float a, b;}, {double d[3];} and a nested
// {{double x, y;}; double z;}; by value across objects, in both directions.
// PEER: abi-structs-peer.cpp
// EXIT: 0
struct D5 { double a, b, c, d, e; };
struct DI { double d; int i; };
struct DF { double d; float f; };
struct FI { float f; int i; };
struct FF { float a, b; };
struct Arr { double d[3]; };
struct P2 { double x, y; };
struct Nested { P2 p; double z; };

double peer_d5(D5 s, double k);
D5 peer_make_d5(double base);
double peer_mixed(DI a, DF b, FI c, FF d);
DI peer_make_di(double d, int i);
DF peer_make_df(double d, float f);
FI peer_make_fi(float f, int i);
FF peer_make_ff(float a, float b);
Arr peer_scale_arr(Arr a, double k);
Nested peer_shift(Nested n, double k);
int peer_calls_main();

double main_d5(D5 s, double k) { return (s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * s.e) * k; }
D5 main_make_d5(double base) { return {base, base + 1, base + 2, base + 3, base + 4}; }
double main_mixed(DI a, DF b, FI c, FF d) {
  return a.d + a.i + 10 * (b.d + b.f) + 100 * (c.f + c.i) + 1000 * (d.a + d.b);
}
DF main_make_df(double d, float f) { return {d, f}; }
FI main_make_fi(float f, int i) { return {f, i}; }
Nested main_shift(Nested n, double k) { return {{n.p.x + k, n.p.y + k}, n.z + k}; }

int main() {
  if (peer_d5({1, 1, 1, 1, 1}, 0.5) != 7.5) return 1;
  D5 m = peer_make_d5(0.5);
  if (m.a != 0.5 || m.c != 1.5 || m.e != 2.5) return 2;
  if (peer_mixed({0.5, 1}, {0.25, 0.75f}, {0.5f, 2}, {1.5f, 2.5f}) != 4261.5) return 3;
  DI di = peer_make_di(2.5, -7);
  if (di.d != 2.5 || di.i != -7) return 4;
  DF df = peer_make_df(-0.125, 3.5f);
  if (df.d != -0.125 || df.f != 3.5f) return 5;
  FI fi = peer_make_fi(0.75f, 42);
  if (fi.f != 0.75f || fi.i != 42) return 6;
  FF ff = peer_make_ff(1.25f, -1.25f);
  if (ff.a != 1.25f || ff.b != -1.25f) return 7;
  Arr a = peer_scale_arr({{1, 2, 3}}, 0.5);
  if (a.d[0] != 0.5 || a.d[1] != 1 || a.d[2] != 1.5) return 8;
  Nested n = peer_shift({{1, 2}, 3}, 0.5);
  if (n.p.x != 1.5 || n.p.y != 2.5 || n.z != 3.5) return 9;
  int r = peer_calls_main();
  return r == 0 ? 0 : 10 + r;
}
