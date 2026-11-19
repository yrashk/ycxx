// θ step 3 (#281): a pack capture names a function parameter pack ([expr.prim.lambda.capture]/17).
int main() {
  int x = 1;
  auto f = [x...] { return 0; }; //! error
  return f();
}
