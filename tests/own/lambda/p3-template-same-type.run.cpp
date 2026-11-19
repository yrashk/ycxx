// θ step 3 (#281), spec-only: in a template lambda `[]<class T>(T a, T b)` both arguments deduce the
// same T ([expr.prim.lambda.closure], [temp.deduct.call], [temp.deduct.type]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

struct Box { int v; };

int main() {
  auto max = []<class T>(T a, T b) { return a < b ? b : a; };
  if (max(3, 9) != 9) return 1;
  if (max('z', 'a') != 'z') return 2;
  if (max(-5L, -7L) != -5L) return 3;
  if (!same<decltype(max('a', 'b')), char>::value) return 4;
  auto first = []<class T>(T a, T) { return a; };
  if (first(Box{4}, Box{5}).v != 4) return 5;
  int i = 1, j = 2;
  if (*max(&i, &i) != 1) return 6;
  auto swap = []<class T>(T& a, T& b) { T t = a; a = b; b = t; };
  swap(i, j);
  if (i != 2 || j != 1) return 7;
  return 0;
}
