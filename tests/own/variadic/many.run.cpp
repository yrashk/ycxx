// More arguments than argument registers: the named format string still goes in a register and
// every argument the ellipsis matches gets its own 8-byte stack slot on Apple arm64, in order.
// STDOUT: 1 2 3 4 5 6 7 8 9 10 11
// STDOUT: a=-1 b=2 c=x d=-4 e=5000000000 f=six g=7 h=8 i=9
// EXIT: 0
extern "C" int printf(const char*, ...);

int main() {
  int n = 11;
  printf("%d %d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, n);
  char word[4] = {'s', 'i', 'x', 0};
  short d = -4;
  long e = 5000000000L;
  int r = printf("a=%d b=%u c=%c d=%hd e=%ld f=%s g=%d h=%ld i=%d\n", -1, 2u, 'x', d, e, word, 7, 8L, 9);
  return r - 49;
}
