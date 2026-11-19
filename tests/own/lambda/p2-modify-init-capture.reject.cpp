// θ step 2 (#280), spec-only: a by-copy init-capture cannot be modified in a lambda without `mutable`
// ([expr.prim.lambda.closure], [expr.prim.lambda.capture]).
int main() {
  auto f = [n = 0] { return ++n; }; //! error
  return f();
}
