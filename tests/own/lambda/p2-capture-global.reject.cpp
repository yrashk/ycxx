// θ step 2 (#280), spec-only: the identifier of a simple-capture must name a local entity; a
// namespace-scope variable cannot be captured ([expr.prim.lambda.capture], [basic.scope]).
int g = 1;
int main() {
  auto f = [g] { return g; }; //! error
  return f();
}
