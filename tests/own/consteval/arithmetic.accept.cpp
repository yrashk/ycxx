// [expr.const], [dcl.constexpr], [dcl.pre]: constexpr functions with arithmetic, loops and
// recursion are evaluated in constant expressions checked by static_assert.
constexpr int square(int x) { return x * x; }
constexpr long fact(long n) { return n <= 1 ? 1 : n * fact(n - 1); }
constexpr int fib(int n) {
  int a = 0;
  int b = 1;
  for (int i = 0; i < n; ++i) {
    int t = a + b;
    a = b;
    b = t;
  }
  return a;
}
constexpr int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }
constexpr unsigned wrap() {
  unsigned u = 0;
  u = u - 1;
  return u + 2;
}
constexpr int digits(int n) {
  int count = 0;
  do {
    n = n / 10;
    ++count;
  } while (n != 0);
  return count;
}
constexpr int forty_two = square(6) + 6;
static_assert(forty_two == 42);
static_assert(fact(10) == 3628800, "10!");
static_assert(fib(10) == 55);
static_assert(gcd(84, 36) == 12);
static_assert(wrap() == 1u);
static_assert(digits(12345) == 5 && digits(0) == 1);
static_assert(-7 / 2 == -3 && -7 % 2 == -1);
static_assert((1 << 4) == 16 && (256 >> 3) == 32);
