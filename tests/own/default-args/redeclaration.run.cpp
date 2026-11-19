// ι (#300): later declarations add default arguments to a function. After each declaration the
// call may omit what the defaults known so far cover; f's definition adds none and keeps them
// all, h's adds the last one. A default may precede a parameter without one in the same
// declaration when an earlier declaration gave that parameter its default ([dcl.fct.default]/4).
// EXIT: 0
int f(int a, int b, int c);
int g0() { return f(1, 2, 3); }

int f(int a, int b, int c = 3);
int g1() { return f(1, 2); }

int f(int a, int b = 2, int c);
int g2() { return f(1); }

int f(int a = 1, int b, int c);
int g3() { return f(); }

int f(int a, int b, int c) { return a * 100 + b * 10 + c; }

int h(int x, int y = 7);
int h(int x = 5, int y) { return x + y; }  // the definition adds the last default

int main() {
  if (g0() != 123) return 1;
  if (g1() != 123) return 2;
  if (g2() != 123) return 3;
  if (g3() != 123) return 4;
  if (f() != 123) return 5;
  if (f(4) != 423) return 6;
  if (f(4, 5) != 453) return 7;
  if (f(4, 5, 6) != 456) return 8;
  if (h() != 12) return 9;
  if (h(1) != 8) return 10;
  if (h(1, 1) != 2) return 11;
  return 0;
}
