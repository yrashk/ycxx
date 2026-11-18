// δ s3 (#86): overload resolution between ordinary functions by exact match, by integral
// promotion over integral conversion, by qualification adjustment, and by arity
// ([over.match.best], [over.ics.rank], [conv.prom]).
// EXIT: 0
int f(int) { return 1; }
int f(long) { return 2; }
int f(unsigned) { return 3; }
int f(char) { return 4; }

int g(long) { return 10; }
int g(long, int) { return 11; }

int h(int*) { return 20; }
int h(const int*) { return 21; }

int k(bool) { return 30; }
int k(int*) { return 31; }

int main() {
  short s = 1;
  unsigned char uc = 2;
  bool b = false;
  long l = 3;
  if (f(0) != 1) return 1;
  if (f(s) != 1) return 2;   // short -> int is a promotion
  if (f(uc) != 1) return 3;  // unsigned char -> int is a promotion
  if (f(b) != 1) return 4;   // bool -> int is a promotion
  if (f('x') != 4) return 5;
  if (f(l) != 2) return 6;
  if (f(5u) != 3) return 7;
  if (f(5L) != 2) return 8;

  if (g(1) != 10) return 9;
  if (g(1, 2) != 11) return 10;

  int x = 0;
  const int cx = 0;
  if (h(&x) != 20) return 11;
  if (h(&cx) != 21) return 12;

  if (k(&x) != 31) return 13;
  if (k(true) != 30) return 14;
  return 0;
}
