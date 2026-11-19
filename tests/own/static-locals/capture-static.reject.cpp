// ι (#291), from the review: a variable with static storage duration cannot be captured
// explicitly ([expr.prim.lambda.capture]/5).
int g() {
  static int c = 0;
  auto l = [c] { return c; }; //! error
  return l();
}
int main() { return g(); }
