// The other side of impl-mangle.run.cpp, compiled by Clang: ycxx's closure members called by the
// names Clang gives them.
// The object-file name of an Itanium symbol: Mach-O prefixes C names with `_`, ELF does not.
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
#define LABEL(name) __asm__(LABEL_PREFIX(__USER_LABEL_PREFIX__) name)
struct Empty {};
int a_f(const Empty*, void*, void*) LABEL("_ZNK1A1fMUlPS_PS1_E_clES1_S2_");
int a_g(const Empty*, int, void*) LABEL("_ZNK1A1gMUlT_PS_E_clIiEEDaS1_S2_");
int a_two(const Empty*, int) LABEL("_ZNK1A3twoMUliE_clEi");
int a_two_inner(const Empty*, int) LABEL("_ZZNK1A3twoMUliE_clEiENKUliE_clEi");
int a_p(int) LABEL("_ZN1A1pMUliE_8__invokeEi");
int b_f(const Empty*, int, void*) LABEL("_ZNK1N1BIiE1fMUliPNS_1QEE_clEiS4_");
int b_g(const Empty*, int, void*) LABEL("_ZNK1N1BIPNS_1QEE1gMUlT_S2_E_clIiEEDaS5_S2_");
int n_v(const Empty*, int, void*) LABEL("_ZNK1N1vMUliPNS_1QEE_clEiS2_");
int n_w(const Empty*, int, void*) LABEL("_ZNK1N1wMUlT_PNS_1QEE_clIiEEDaS1_S3_");
int n_vt(const Empty*, int, void*) LABEL("_ZNK1N2vtIiEMUliPNS_1QEE_clEiS3_");
int n_gt(const Empty*, int, void*) LABEL("_ZNK1N2gtIiEMUlT_PNS_1QEE_clIiEEDaS2_S4_");
int peer_check() {
  Empty e;
  if (a_f(&e, &e, &e) != 1 || a_g(&e, 1, &e) != 3 || a_two(&e, 1) != 4 || a_two_inner(&e, 1) != 4 || a_p(1) != 5) return 4;
  if (b_f(&e, 1, &e) != 6 || b_g(&e, 1, &e) != 7) return 5;
  return n_v(&e, 1, &e) == 8 && n_w(&e, 1, &e) == 9 && n_vt(&e, 1, &e) == 10 && n_gt(&e, 1, &e) == 11 ? 0 : 6;
}
