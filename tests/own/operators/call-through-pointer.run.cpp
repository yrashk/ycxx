// θ step 1 (#279): calls through pointers to functions and references to functions
// ([expr.call]/1): arguments, a class result, and in constant evaluation.
// STDOUT: 5 5
struct P { int a, b; };
P mk(int a) { return {a, a + 1}; }
int add(int a, int b) { return a + b; }
int total(int k, P p) { return k + p.a + p.b; }
extern "C" int printf(const char*, ...);
constexpr int sq(int x) { return x * x; }
constexpr int via(int (*f)(int), int v) { return f(v); }
static_assert(via(sq, 4) == 16);
int main() {
  int (*f)(int, int) = add;
  P (*m)(int) = &mk;
  int (&r)(int, int) = add;
  printf("%d %d\n", f(2, 3), m(4).b);
  P p = m(7);
  int (*t)(int, P) = total;
  if (r(1, 2) != 3 || (*f)(3, 4) != 7 || t(1, P{2, 3}) != 6) return 2;
  return p.a + p.b == 15 ? 0 : 1;
}
