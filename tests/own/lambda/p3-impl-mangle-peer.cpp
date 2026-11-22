// The other side of p3-impl-mangle.run.cpp, compiled by Clang: ycxx's specializations called by
// the names Clang gives them.
// The object-file name of an Itanium symbol: Mach-O prefixes C names with `_`, ELF does not.
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
#define LABEL(name) __asm__(LABEL_PREFIX(__USER_LABEL_PREFIX__) name)
struct Empty {};
struct V { int v; };
int a_int(const Empty*, int) LABEL("_ZZ1kvENKUlT_E_clIiEEDaS_");
long a_long(const Empty*, long) LABEL("_ZZ1kvENKUlT_E_clIlEEDaS_");
long b_long(const Empty*, long, int) LABEL("_ZZ1kvENKUlTyT_iE_clIlEEDaS_i");
int c_int(const Empty*, int, int&) LABEL("_ZZ1kvENKUlT_RT0_E_clIiiEEDaS_S1_");
int g_int(const V*, int) LABEL("_ZZ1tIiEiT_ENKUlS0_E_clIiEEDaS0_");
long s_il(const Empty*, int, long) LABEL("_ZZ5packsvENKUlDpT_E_clIJilEEEDaS0_");
unsigned long c_ic(const Empty*, int, char) LABEL("_ZZ5packsvENKUlTpTyDpT_E_clIJicEEEDaS0_");
int m_ii(const Empty*, int, int) LABEL("_ZZ5packsvENKUliDpT_E_clIJiEEEDaiS0_");
int d_i(const Empty*, int) LABEL("_ZZ5headsvENKUlT_E_clIiEEDaS_");
int e_i(const Empty*, int) LABEL("_ZZ5headsvENKUlTyT_E_clIiEEDaS_");
int f_i(const Empty*, int) LABEL("_ZZ5headsvENKUlT_E0_clIiEEDaS_");
int u_i(const Empty*, int) LABEL("_ZZ5headsvENKUlTyiE_clIiEEDai");
int peer_check() {
  Empty e;
  V v{5};
  int z = 4;
  if (a_int(&e, 1) != 2 || a_long(&e, 2) != 3 || b_long(&e, 3, 4) != 12) return 2;
  if (c_int(&e, 1, z) != 5 || g_int(&v, 2) != 7) return 3;
  if (d_i(&e, 1) != 1 || e_i(&e, 2) != 2 || f_i(&e, 3) != 4 || u_i(&e, 4) != 4) return 5;
  if (s_il(&e, 1, 2) != 3 || c_ic(&e, 1, 'a') != 2 || m_ii(&e, 2, 3) != 8) return 4;
  return 0;
}
