// The other side of classes-by-value.run.cpp, compiled by Clang: functions taking and returning
// classes of every passing kind, and calls back into ycxx's.
struct S8 { int a, b; };
struct S12 { int a, b, c; };
struct S16 { long a, b; };
struct S24 { long a, b, c; };
struct S3 { char a, b, c; };
struct NT { int v; NT(int x) : v(x) {} NT(const NT& o) : v(o.v + 100) {} ~NT() {} };
S8 peer_s8(S8 x, int k) { return {x.a + k, x.b + k}; }
S12 peer_s12(S12 x) { return {x.c, x.b, x.a}; }
S16 peer_s16(S16 x, S16 y) { return {x.a + y.b, x.b + y.a}; }
S24 peer_s24(S24 x) { return {x.a * 2, x.b * 2, x.c * 2}; }
S3 peer_s3(S3 x) { return {x.c, x.b, x.a}; }
int peer_nt(NT n) { return n.v; }
NT peer_make_nt(int v) { return NT(v); }
// Calls back into the other compiler's functions.
S12 main_s12(S12 x);
S24 main_s24(S24 x, int k);
int peer_calls_back() {
  S12 r = main_s12({1, 2, 3});
  S24 t = main_s24({1, 2, 3}, 10);
  return r.a + r.b * 10 + r.c * 100 + int(t.a + t.b + t.c);  // 321-ish + 36
}
