// θ step 2 (#280), from the review: a lambda-specifier appears at most once
// ([expr.prim.lambda.general]/3).
int main() {
  auto f = []() mutable mutable { return 1; }; //! error
  return f();
}
