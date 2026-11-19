// θ step 2 (#280), spec-only: with the capture-default `&`, no simple-capture may be preceded by `&`
// ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  auto f = [&, &x] { return x; }; //! error
  return f();
}
