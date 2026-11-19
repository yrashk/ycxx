// θ step 2 (#280), spec-only: a closure object stored in a member of a class template specialized on the
// closure type, and called through it ([expr.prim.lambda.closure], [temp.arg.type], [dcl.init.aggr]).
template<class F> struct Holder {
  F f;
  int call(int x) { return f(x); }
};

template<class F> Holder<F> hold(F f) { return Holder<F>{f}; }

int main() {
  int k = 3;
  auto l = [k](int x) { return x * k; };
  Holder<decltype(l)> h{l};
  if (h.call(5) != 15) return 1;
  if (h.f(2) != 6) return 2;
  auto g = hold([&k](int x) { return x + k; });
  k = 30;
  if (g.call(1) != 31) return 3;
  auto m = hold([n = 0](int x) mutable { n += x; return n; });
  m.call(2);
  if (m.call(3) != 5) return 4;
  return 0;
}
