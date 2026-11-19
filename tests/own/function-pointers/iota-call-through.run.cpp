// ι (#313): calls through pointers and references to functions: `p(…)`, `(*p)(…)`, `(**p)(…)`
// (a function designator converts back to a pointer), `r(…)`, and arrays of function pointers
// initialized from overload sets ([expr.call], [expr.unary.op], [conv.func], [dcl.init.aggr]).
// STDOUT: abc
// EXIT: 0
extern "C" int putchar(int);

int f(int x) { return x + 1; }
int f(long x) { return static_cast<int>(x) + 2; }
int g(int x) { return x * 2; }
int h(int x) { return x * x; }

void put_a() { putchar('a'); }
void put_b() { putchar('b'); }
void put_c() { putchar('c'); }

int main() {
  int (*p)(int) = f;
  if (p(1) != 2) return 1;
  if ((*p)(1) != 2) return 2;
  if ((**p)(1) != 2) return 3;
  if ((***&*p)(1) != 2) return 4;
  int (&r)(int) = *p;
  if (r(1) != 2) return 5;
  if ((&r)(1) != 2) return 6;
  if ((*r)(1) != 2) return 7;
  int (*table[3])(int) = {f, g, &h};
  int sum = 0;
  for (int i = 0; i < 3; ++i) sum += table[i](3);
  if (sum != 4 + 6 + 9) return 8;
  if ((*table[2])(4) != 16) return 9;
  int (*longs[])(long) = {f, &f};
  if (longs[1](0) != 2) return 10;
  void (*out[])() = {put_a, &put_b, put_c};
  for (int i = 0; i < 3; ++i) out[i]();
  putchar('\n');
  return 0;
}
