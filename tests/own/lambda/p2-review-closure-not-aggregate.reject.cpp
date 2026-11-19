// θ step 2 (#280), from the review: a closure type is not an aggregate, and its captures'
// members have no names ([expr.prim.lambda.closure]/1, [expr.prim.lambda.capture]/10).
int main() {
  int x = 1;
  auto l = [x] { return x; };
  return l.x; //! error
}
