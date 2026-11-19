// θ step 2 (#280), spec-only: the closure type of a lambda-expression with a lambda-capture has no
// default constructor ([expr.prim.lambda.closure]).
int main() {
  int k = 1;
  auto f = [k] { return k; };
  decltype(f) g; //! error
  return g();
}
