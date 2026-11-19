// θ step 3 (#281), spec-only: calling a generic lambda instantiates its function call operator for the
// argument type; a body that is ill-formed for int (member access on a non-class) is an error in that
// specialization ([expr.prim.lambda.closure], [temp.inst], [expr.ref]).
int main() {
  auto f = [](auto x) {
    return x.foo(); //! error
  };
  return f(1);
}
