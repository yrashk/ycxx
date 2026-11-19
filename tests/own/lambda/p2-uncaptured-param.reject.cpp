// θ step 2 (#280), spec-only: a function parameter is a local entity too; a lambda that captures other
// variables but not it cannot use it ([expr.prim.lambda.capture], [basic.def.odr]).
int f(int p, int q) {
  auto g = [q] { return q + p; }; //! error
  return g();
}
int main() { return f(1, 2); }
