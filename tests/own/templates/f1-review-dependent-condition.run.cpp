// ζ f1 (#88), from the step's review: a conditional expression whose condition alone is
// value-dependent is value-dependent ([temp.dep.constexpr]), in a static_assert, a constexpr
// local and an array bound.
// EXIT: 0
template<int N> int f() {
  static_assert((N > 0 ? 1 : 0) == 1);
  constexpr int k = N > 2 ? 3 : 4;
  int a[N > 2 ? 3 : 4];
  a[0] = k;
  return a[0];
}
int main() {
  if (f<5>() != 3) return 1;
  if (f<1>() != 4) return 2;
  return 0;
}
