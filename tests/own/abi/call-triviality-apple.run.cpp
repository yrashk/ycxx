// #230 (DECISIONS §1.6): `-fclass-call-abi=apple`, which classes pass in registers as Apple Clang
// has it on arm64 Darwin: a trivial destructor and a copy constructor trivial or deleted, whatever
// the move constructors. TC, MV, DC, DM, WM, WD and WA go in registers; NC and ND by address. The same
// calls as call-triviality.run.cpp, against Apple Clang's objects.
// XFAIL: gcc the option is ycxx's
// XFAIL: clang the option is ycxx's
// FLAGS: -fclass-call-abi=apple
// PEER-WITH: apple-clang
// REQUIRES: darwin
// PARITY: by design: -fclass-call-abi=apple matches Apple Clang on arm64 Darwin (DECISIONS §1.6)
// PEER: call-triviality-peer.cpp
// EXIT: 0
struct TC { int v; TC(int x) : v(x) {} };
struct NC { int v; NC(int x) : v(x) {} NC(const NC& o) : v(o.v) {} };
struct ND { int v; ND(int x) : v(x) {} ~ND() {} };
struct MV { int v; MV(int x) : v(x) {} MV(MV&& o) : v(o.v) {} };
struct DC { int v; DC(int x) : v(x) {} DC(const DC&) = delete; };
struct DM { int v; DM(int x) : v(x) {} DM(const DM&) = delete; DM(DM&& o) : v(o.v) {} };
struct WM { MV m; int k; };
struct WD { DC m; int k; };
struct WA { DC m[2]; int k; };
int p_tc(TC a, int k); int p_nc(NC a, int k); int p_nd(ND a, int k); int p_mv(MV a, int k);
int p_dc(DC a, int k); int p_dm(DM a, int k); int p_wm(WM a); int p_wd(WD a); int p_wa(WA a);
TC r_tc(); NC r_nc(); ND r_nd(); MV r_mv(); DC r_dc(); DM r_dm(); WM r_wm();
int peer_calls();
int m_tc(TC a, int k) { return a.v * 10 + k; }
int m_nc(NC a, int k) { return a.v * 10 + k; }
int m_nd(ND a, int k) { return a.v * 10 + k; }
int m_mv(MV a, int k) { return a.v * 10 + k; }
int m_dc(DC a, int k) { return a.v * 10 + k; }
int m_dm(DM a, int k) { return a.v * 10 + k; }
int m_wm(WM a) { return a.m.v * 10 + a.k; }
int m_wd(WD a) { return a.m.v * 10 + a.k; }
int m_wa(WA a) { return a.m[1].v * 10 + a.k; }
int main() {
  if (p_tc(TC(1), 2) != 12) return 1;
  if (p_nc(NC(1), 3) != 13) return 2;
  if (p_nd(ND(1), 4) != 14) return 3;
  if (p_mv(MV(1), 5) != 15) return 4;
  if (p_dc(DC(1), 6) != 16) return 5;
  if (p_dm(DM(1), 7) != 17) return 6;
  if (p_wm(WM{MV(1), 8}) != 18) return 7;
  if (p_wd(WD{DC(1), 9}) != 19) return 15;
  if (p_wa(WA{{DC(0), DC(2)}, 1}) != 21) return 16;
  // One per statement: a temporary with a destructor in an operand of `||` is #157's.
  if (r_tc().v != 21) return 8;
  if (r_nc().v != 22) return 9;
  if (r_nd().v != 23) return 10;
  if (r_mv().v != 24) return 11;
  if (r_dc().v != 25) return 12;
  if (r_dm().v != 26) return 13;
  WM w = r_wm();
  if (w.m.v != 27 || w.k != 28) return 14;
  return peer_calls();
}
