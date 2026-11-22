// The other side of p1-impl-mangle.run.cpp, compiled by Clang: the specializations ycxx defined,
// called by the names Clang gives them.
// The object-file name of an Itanium symbol: Mach-O prefixes C names with `_`, ELF does not.
#define LABEL_STR(p) #p
#define LABEL_PREFIX(p) LABEL_STR(p)
#define LABEL(name) __asm__(LABEL_PREFIX(__USER_LABEL_PREFIX__) name)
int twice_int(int) LABEL("_Z5twiceIiEDaT_");
long twice_long(long) LABEL("_Z5twiceIlEDaT_");
int& first_int(int&) LABEL("_Z5firstIiEDcRT_");
const int& id_int(const int&) LABEL("_Z2idIiERKDaRKT_");
int peer_check() {
  int v = 7;
  first_int(v) = 8;
  if (twice_int(5) != 10 || twice_long(6) != 12 || v != 8 || id_int(9) != 9) return 2;
  return 0;
}
