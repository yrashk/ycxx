// The scan is indeterminately sequenced with the other operand ([intro.execution]).
constexpr bool scan_body() {
  char a[] = "abc";
  int k = 1;
  auto n = (__builtin_strlen(a) + k) + (a[0] = 'x', 0);
  return n == 4;
}
static_assert(scan_body());

constexpr bool ordered_argument() {
  const char* a = "abc";
  const char* p = a;
  auto n = __builtin_strlen(p++);
  p = a + 1;
  return n == 3 && p == a + 1;
}
static_assert(ordered_argument());
