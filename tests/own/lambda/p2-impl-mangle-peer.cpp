// The other side of p2-impl-mangle.run.cpp, compiled by Clang: ycxx's closure members called by
// the names Clang gives them, on closure objects laid out as Clang lays them out.
// The object-file name of an Itanium symbol: Mach-O prefixes C names with `_`, ELF does not.
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
#define LABEL(name) __asm__(LABEL_PREFIX(__USER_LABEL_PREFIX__) name)
struct S { int v; };
struct Empty {};
struct GInt { int t; };
struct Run { S* self; int k; };
int a_call(const Empty*) LABEL("_ZZ1fvENKUlvE_clEv");
int b_call(const Empty*, int) LABEL("_ZZ1fvENKUliE_clEi");
int c_call(const Empty*) LABEL("_ZZ1fvENKUlvE0_clEv");
int inner_call(const Empty*) LABEL("_ZZ1fvENKUlvE1_clEv");
int outer_call(const Empty*) LABEL("_ZZ1fvENKUlvE2_clEv");
int g_call(const GInt*, int) LABEL("_ZZ1gIiET_S0_ENKUliE_clEi");
int h_invoke(int) LABEL("_ZZ1hvENUliE_8__invokeEi");
int run_call(const Run*, int) LABEL("_ZZN1S3runEiENKUliE_clEi");
int peer_check() {
  Empty e;
  GInt gi{3};
  S s{10};
  Run r{&s, 20};
  if (a_call(&e) != 1 || b_call(&e, 5) != 5 || c_call(&e) != 2) return 2;
  if (inner_call(&e) != 7 || outer_call(&e) != 8) return 3;
  if (g_call(&gi, 4) != 7 || h_invoke(2) != 6) return 4;
  if (run_call(&r, 1) != 31) return 5;
  return 0;
}
