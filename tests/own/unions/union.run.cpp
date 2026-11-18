// #133: unions ([class.union]): every member at the union's address, the size of the largest,
// aggregate initialization of the first member ([dcl.init.aggr]/16), copies, members of
// structs, an unnamed union named by a typedef as the SDK's __mbstate_t is.
// EXIT: 0
typedef union {
  char m8[128];
  long long ml;
} mbstate;
union U { int i; unsigned char b[4]; short s; };
union U g = {0x01020304};
struct Holder { int tag; U u; };
struct After { mbstate m; int after; };
int main() {
  U a{7};
  a.s = 3;
  U b = a;
  mbstate st{};
  Holder h{1, {5}};
  h.u.i = 9;
  U z{};
  After af{};
  af.m.m8[127] = 1;
  af.after = 2;
  af.m.ml = 3;  // overlaps m8[0..7] only
  return (g.b[0] - 4) + (b.i & 0xffff) - 3 + st.m8[100] + h.u.i - 9 + h.tag - 1 + z.i + af.m.m8[127] - 1 + af.after - 2 + (af.m.m8[0] - 3);
}
