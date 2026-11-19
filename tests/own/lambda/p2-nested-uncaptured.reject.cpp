// θ step 2 (#280), spec-only: an inner lambda's capture of a variable needs every enclosing lambda to
// capture it as well; the outer lambda has neither a capture-default nor an explicit capture
// ([expr.prim.lambda.capture]).
int main() {
  int x = 1;
  auto outer = [] {
    return [&] { return x; }(); //! error
  };
  return outer();
}
