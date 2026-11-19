// θ step 3 (#281), from the review: what a generic lambda's specialization uses is
// instantiated: a function template, a class template's member ([temp.inst]/4, /8); a lambda in
// a generic lambda's body is code only in its specializations; a generic lambda in another
// lambda's body is called there; a template lambda whose signature names no template parameter.
template<class T> T twice(T x) { return x * 2; }
template<class T> struct B { T v; T get() const { return v; } };
int main() {
  auto f = [](auto x) { return twice(x); };
  auto c = [](auto x) { B<decltype(x)> b{x}; return b.get(); };
  if (f(3) != 6 || c(3L) != 3L) return 1;
  auto outer = [](auto x) { auto in = [] { return 1; }; return in() + x; };
  (void)outer;
  auto nest = [](int x) { auto in = [](auto y) { return y + 1; }; return in(x); };
  if (nest(1) != 2) return 2;
  auto nd = []<class T = long>(int x) { T y = x; return sizeof(y); };
  if (nd(1) != sizeof(long)) return 3;
  auto dflt = []<int N = 3>(auto x) { return x + N; };
  auto arr = [](int (&a)[3]) { return a[2]; };
  int xs[3] = {1, 2, 3};
  return dflt(1) == 4 && arr(xs) == 3 ? 0 : 4;
}
