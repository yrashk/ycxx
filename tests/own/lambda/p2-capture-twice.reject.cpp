// θ step 2 (#280), spec-only: an identifier shall not appear more than once in a lambda-capture
// ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  auto f = [x, x] { return x; }; //! error
  return f();
}
