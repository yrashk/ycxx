// [expr.const], [basic.types], [dcl.array], [dcl.pre]: a const integral variable with a constant
// initializer is usable in constant expressions, as an array bound and in static_assert, at
// namespace and block scope.
const int global_size = 4;
const char letter = 'a';
int global_array[global_size];
static_assert(global_size == 4);
static_assert(letter == 'a');
constexpr int twice(int n) { return n * 2; }
int fill() {
  const int n = 3;
  const int m = n + 2;
  const long big = twice(m);
  int a[n];
  char b[m];
  int c[big];
  static_assert(n == 3 && m == 5);
  static_assert(big == 10, "twice(5)");
  constexpr int k = n * m;
  static_assert(k == 15);
  const bool flag = n < m;
  static_assert(flag);
  a[0] = 1;
  b[0] = letter;
  c[9] = 2;
  return a[0] + c[9];
}
