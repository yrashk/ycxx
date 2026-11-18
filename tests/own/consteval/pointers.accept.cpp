// [expr.const], [expr.add], [expr.unary.op], [dcl.array]: pointers and arrays of locals, string
// literals, and one-past-the-end pointers are fine in constant evaluation.
constexpr int sum_to(int n) {
  int a[10];
  for (int i = 0; i < 10; ++i)
    a[i] = i;
  int s = 0;
  for (int* p = a; p != a + 10; ++p)
    if (*p <= n)
      s += *p;
  return s;
}
constexpr int length(const char* s) {
  int n = 0;
  while (s[n] != 0)
    ++n;
  return n;
}
constexpr void set(int* p, int v) { *p = v; }
constexpr int through_pointer() {
  int x = 1;
  set(&x, 9);
  int* q = &x;
  return *q + 1;
}
constexpr char third(const char* s) { return *(s + 2); }
static_assert(sum_to(4) == 10);
static_assert(length("hello") == 5);
static_assert(through_pointer() == 10);
static_assert(third("abcd") == 'c');
static_assert("xyz"[1] == 'y');
