// [expr.const], [dcl.constexpr]: an immediate invocation must be a constant expression; a
// parameter of a function that is not immediate is not constant, and an evaluation with
// undefined behavior is not constant either.
consteval int sq(int n) { return n * n; }
consteval int zero() { return 0; }
int global = 3;
int from_param(int n) {
  return sq(n); //! error
}
constexpr int from_constexpr_param(int n) {
  return sq(n); //! error
}
int fine(int n) { return n + sq(2); }
int from_global = sq(global); //! error
int div_zero = sq(1 / zero()); //! error
consteval int inverse(int n) { return 100 / n; } int bad_inverse = inverse(0); //! error
int div_ok = inverse(4);
void local() {
  int x = 2;
  const int k = 3;
  int a = sq(k);
  int b = sq(x); //! error
}
