// The default argument promotions ([expr.call]/12, [conv.prom]): bool, char, signed char,
// unsigned char, short and unsigned short arguments matched by an ellipsis become int; wider ones
// keep their type.
// STDOUT: A -1 -3 65535 1 255
// STDOUT: -2147483648 4294967295 -4294967296
// EXIT: 0
extern "C" int printf(const char*, ...);

int main() {
  char c = 'A';
  signed char sc = -1;
  short s = -3;
  unsigned short us = 65535;
  bool b = true;
  unsigned char uc = 255;
  printf("%c %d %d %d %d %d\n", c, sc, s, us, b, uc);
  int imin = -2147483647 - 1;
  unsigned u = 4294967295u;
  long l = -4294967296L;
  printf("%d %u %ld\n", imin, u, l);
  return 0;
}
