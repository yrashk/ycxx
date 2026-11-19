// θ step 2 (#280), spec-only: a local variable odr-used in a lambda with no capture-default must be
// captured explicitly ([expr.prim.lambda.capture], [basic.def.odr]).
int main() {
  int x = 1;
  auto f = [] { return x; }; //! error
  return f();
}
