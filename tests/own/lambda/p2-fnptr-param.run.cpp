// θ step 2 (#280), spec-only: a captureless lambda passed where a parameter of pointer-to-function type
// is expected converts implicitly ([expr.prim.lambda.closure], [over.ics.user], [expr.call]).
int apply(int (*f)(int), int x) { return f(x); }
int fold(int (*op)(int, int), const int* a, int n) {
  int acc = a[0];
  for (int i = 1; i < n; ++i) acc = op(acc, a[i]);
  return acc;
}
void each(void (*f)(int&), int* a, int n) {
  for (int i = 0; i < n; ++i) f(a[i]);
}

int main() {
  if (apply([](int v) { return v + 5; }, 1) != 6) return 1;
  int a[4] = {1, 2, 3, 4};
  if (fold([](int x, int y) { return x + y; }, a, 4) != 10) return 2;
  if (fold([](int x, int y) { return x * y; }, a, 4) != 24) return 3;
  each([](int& r) { r = -r; }, a, 4);
  if (a[0] != -1 || a[3] != -4) return 4;
  return 0;
}
