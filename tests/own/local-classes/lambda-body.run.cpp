// Local classes (#117), spec-only: a class defined in a lambda body is a local class of the
// closure's function call operator; it can use the lambda's parameters' types and the captures
// through the lambda body, and is destroyed at the end of the call ([expr.prim.lambda.closure],
// [class.local]).
int main() {
  int log = 0;
  auto f = [&log](int x) {
    struct Step {
      int& out;
      int v;
      int next() const { return v + 1; }
      ~Step() { out += 1000; }
    };
    Step s{log, x};
    return s.next();
  };
  if (f(4) != 5) return 1;
  if (log != 1000) return 2;
  auto g = [](auto v) {
    struct W { decltype(v) inner; };
    return W{v};
  };
  if (g(7).inner != 7) return 3;
  if (g(2.5).inner != 2.5) return 4;
  return 0;
}
