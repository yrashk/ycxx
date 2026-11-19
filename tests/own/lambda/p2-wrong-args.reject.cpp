// θ step 2 (#280), spec-only: calling a closure object with the wrong number of arguments finds no
// viable function call operator ([expr.prim.lambda.closure], [over.call.object], [over.match.viable]).
int main() {
  auto f = [](int a, int b) { return a + b; };
  return f(1); //! error
}
