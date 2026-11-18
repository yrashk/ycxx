// α1 (#39): function declarations, definitions, parameters, calls and recursion ([dcl.fct], [expr.call]).
// EXIT: 55
int fib(int n);

int add(int a, int b) { return a + b; }

int fib(int n) {
  if (n < 2) return n;
  return add(fib(n - 1), fib(n - 2));
}

long gcd(long a, long b) { return b == 0 ? a : gcd(b, a % b); }

int main() {
  if (gcd(1071, 462) != 21) return 1;
  return fib(10);
}
