// θ step 2 (#280), spec-only: with the capture-default `=`, each simple-capture must be `& identifier`,
// `this` or `* this` ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  auto f = [=, x] { return x; }; //! error
  return f();
}
