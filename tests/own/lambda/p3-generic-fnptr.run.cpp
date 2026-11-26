// θ step 3 (#281), spec-only: a captureless generic lambda has a conversion function template to
// pointer to function; the target pointer type deduces the invented template parameters
// ([expr.prim.lambda.closure], [temp.deduct.conv]).
int apply(int (*f)(int), int x) { return f(x); }

int main() {
  auto inc = [](auto x) { return x + 1; };
  int (*pi)(int) = inc;
  long (*pl)(long) = inc;
  if (pi(1) != 2) return 1;
  if (pl(40L) != 41L) return 2;
  if (apply(inc, 9) != 10) return 3;
  auto first = [](auto a, auto) { return a; };
  char (*pc)(char, long) = first;
  if (pc('k', 5L) != 'k') return 4;
  auto tmpl = []<class T>(T a, T b) { return a - b; };
  int (*pt)(int, int) = tmpl;
  if (pt(9, 4) != 5) return 5;
  return 0;
}
