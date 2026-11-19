// θ step 3 (#281), spec-only: a template lambda `[]<class T>(const T& x, int n)` deduces T from the
// first argument only; the second, non-dependent parameter converts its argument to int
// ([expr.prim.lambda.closure], [temp.deduct.call], [temp.arg.explicit], [conv.integral]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct Pt { int x; int y; };

int main() {
  auto times = []<class T>(const T& x, int n) { return x * n; };
  if (times(3, 4) != 12) return 1;
  if (times(3L, '\2') != 6L) return 2;
  if (times('\5', 2L) != 10) return 3;
  if (!same<decltype(times(3L, 1)), long>::value) return 4;
  if (!same<decltype(times('a', 1)), int>::value) return 5;
  auto nth = []<class T>(const T& p, int n) { return n == 0 ? p.x : p.y; };
  Pt p{7, 8};
  if (nth(p, 0) != 7 || nth(p, 1L) != 8) return 6;
  return 0;
}
