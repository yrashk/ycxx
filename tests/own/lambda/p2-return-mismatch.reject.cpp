// θ step 2 (#280), spec-only: the return statements of a lambda without a trailing-return-type must
// deduce the same type ([expr.prim.lambda.closure], [dcl.spec.auto.general]).
int main() {
  auto f = [](bool b) {
    if (b) return 1;
    return 'c'; //! error
  };
  return f(true);
}
