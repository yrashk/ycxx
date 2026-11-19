// θ step 2 (#280), spec-only: with the capture-default `&`, naming a non-static member in the lambda
// implicitly captures `this` ([expr.prim.lambda.capture], [expr.prim.id.general]).
struct Box {
  int v;
  int scale(int k) {
    auto f = [&] { return v * k; };
    v = 5;
    return f();
  }
  void reset() { [&] { v = 0; }(); }
};

int main() {
  Box b{2};
  if (b.scale(3) != 15) return 1;
  b.reset();
  if (b.v != 0) return 2;
  return 0;
}
