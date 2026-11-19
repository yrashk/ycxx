// The other side of call-triviality.run.cpp (compiled by Clang) and call-triviality-apple.run.cpp
// (compiled by Apple Clang).
struct TC { int v; TC(int x) : v(x) {} };
struct NC { int v; NC(int x) : v(x) {} NC(const NC& o) : v(o.v) {} };
struct ND { int v; ND(int x) : v(x) {} ~ND() {} };
struct MV { int v; MV(int x) : v(x) {} MV(MV&& o) : v(o.v) {} };
struct DC { int v; DC(int x) : v(x) {} DC(const DC&) = delete; };
struct DM { int v; DM(int x) : v(x) {} DM(const DM&) = delete; DM(DM&& o) : v(o.v) {} };
struct WM { MV m; int k; };
struct WD { DC m; int k; };
struct WA { DC m[2]; int k; };
int p_tc(TC a, int k) { return a.v * 10 + k; }
int p_nc(NC a, int k) { return a.v * 10 + k; }
int p_nd(ND a, int k) { return a.v * 10 + k; }
int p_mv(MV a, int k) { return a.v * 10 + k; }
int p_dc(DC a, int k) { return a.v * 10 + k; }
int p_dm(DM a, int k) { return a.v * 10 + k; }
int p_wm(WM a) { return a.m.v * 10 + a.k; }
int p_wd(WD a) { return a.m.v * 10 + a.k; }
int p_wa(WA a) { return a.m[1].v * 10 + a.k; }
TC r_tc() { return TC(21); }
NC r_nc() { return NC(22); }
ND r_nd() { return ND(23); }
MV r_mv() { return MV(24); }
DC r_dc() { return DC(25); }
DM r_dm() { return DM(26); }
WM r_wm() { return WM{MV(27), 28}; }
int m_tc(TC a, int k); int m_nc(NC a, int k); int m_nd(ND a, int k); int m_mv(MV a, int k);
int m_dc(DC a, int k); int m_dm(DM a, int k); int m_wm(WM a); int m_wd(WD a); int m_wa(WA a);
int peer_calls() {
  if (m_tc(TC(3), 1) != 31) return 11;
  if (m_nc(NC(3), 2) != 32) return 12;
  if (m_nd(ND(3), 3) != 33) return 13;
  if (m_mv(MV(3), 4) != 34) return 14;
  if (m_dc(DC(3), 5) != 35) return 15;
  if (m_dm(DM(3), 6) != 36) return 16;
  if (m_wm(WM{MV(3), 7}) != 37) return 17;
  if (m_wd(WD{DC(3), 8}) != 38) return 18;
  if (m_wa(WA{{DC(0), DC(3)}, 9}) != 39) return 19;
  return 0;
}
