// θ step 2 (#280), from the review: a closure with a lambda-capture has a deleted copy
// assignment operator ([expr.prim.lambda.closure]/13).
int main() {
  int x = 1;
  auto a = [x] { return x; };
  auto b = a;
  b = a; //! error
  return b();
}
