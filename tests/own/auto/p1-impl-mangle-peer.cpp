// The other side of p1-impl-mangle.run.cpp, compiled by Clang: the specializations ycxx defined,
// called by the names Clang gives them.
int twice_int(int) __asm__("__Z5twiceIiEDaT_");
long twice_long(long) __asm__("__Z5twiceIlEDaT_");
int& first_int(int&) __asm__("__Z5firstIiEDcRT_");
const int& id_int(const int&) __asm__("__Z2idIiERKDaRKT_");
int peer_check() {
  int v = 7;
  first_int(v) = 8;
  if (twice_int(5) != 10 || twice_long(6) != 12 || v != 8 || id_int(9) != 9) return 2;
  return 0;
}
