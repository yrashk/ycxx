// θ step 2 (#280), spec-only: without `mutable` the function call operator is const, so a member
// captured by copy cannot be modified ([expr.prim.lambda.closure], [expr.prim.lambda.capture],
// [expr.assign]).
int main() {
  int x = 1;
  auto f = [x] { x = 2; }; //! error
  f();
  return x;
}
