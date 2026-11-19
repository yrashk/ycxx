// θ step 2 (#280), spec-only: a lambda without return statements returns void, which cannot initialize
// an int ([expr.prim.lambda.closure], [dcl.spec.auto.general], [dcl.init.general]).
int main() {
  auto f = [](int& r) { r = 1; };
  int v = 0;
  int x = f(v); //! error
  return x;
}
