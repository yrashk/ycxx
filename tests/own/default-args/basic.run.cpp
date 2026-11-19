// ι (#300): non-member functions with one and with several trailing default arguments, called
// with none, some and all of the defaulted arguments supplied; a supplied argument replaces the
// default, the omitted ones are taken from the defaults left to right ([dcl.fct.default]/1,
// [expr.call]/5).
// EXIT: 0
int one(int a, int b = 10) { return a + b; }
int three(int a, int b = 20, int c = 300, int d = 4000) { return a + b + c + d; }
int all(int a = 1, int b = 2) { return a * 10 + b; }
char pick(char c = 'z') { return c; }
bool flag(bool b = true) { return b; }
unsigned long wide(unsigned long x = 7ul) { return x; }

int main() {
  if (one(1) != 11) return 1;
  if (one(1, 2) != 3) return 2;
  if (three(1) != 4321) return 3;
  if (three(1, 2) != 4303) return 4;
  if (three(1, 2, 3) != 4006) return 5;
  if (three(1, 2, 3, 4) != 10) return 6;
  if (all() != 12) return 7;
  if (all(5) != 52) return 8;
  if (all(5, 6) != 56) return 9;
  if (pick() != 'z' || pick('a') != 'a') return 10;
  if (!flag() || flag(false)) return 11;
  if (wide() != 7ul || wide(9ul) != 9ul) return 12;
  return 0;
}
