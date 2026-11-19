// θ step 2 (#280), spec-only: a block-scope variable with static storage duration is not a local entity
// and cannot be captured ([expr.prim.lambda.capture], [basic.pre]).
int main() {
  static int s = 1;
  auto f = [s] { return s; }; //! error
  return f();
}
