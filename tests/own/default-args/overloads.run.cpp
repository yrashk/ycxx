// ι (#300): overload resolution with default arguments. A function with more parameters than
// arguments is viable only when the extra parameters have defaults, and only the parameters
// that receive arguments take part in ranking ([over.match.viable]/2, [over.match.best],
// [dcl.fct.default]/1).
// EXIT: 0
int f(int) { return 1; }
int f(int, int = 0) { return 2; }

int g(int, int = 1) { return 10; }
int g(long) { return 20; }

int h(const char*) { return 100; }
int h(int, char = 'c', bool = true) { return 200; }  // one argument: viable by its defaults

int k(int a, int b) { return a + b; }
int k(long a, int b = 5) { return static_cast<int>(a) * b; }

int main() {
  if (f(1, 2) != 2) return 1;  // only the two-parameter f is viable
  if (g(1) != 10) return 2;    // an exact match for the one argument; the default is not ranked
  if (g(1L) != 20) return 3;
  if (h("x") != 100) return 4;
  if (h(3) != 200) return 5;
  if (h(3, 'a') != 200) return 6;
  if (k(2, 3) != 5) return 7;
  if (k(2) != 10) return 8;    // only k(long, int = 5) takes one argument
  return 0;
}
