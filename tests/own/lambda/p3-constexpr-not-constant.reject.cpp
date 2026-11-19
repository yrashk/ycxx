// θ step 3 (#281), spec-only: a lambda whose call reads a non-const global is not a core constant
// expression, so it cannot initialize a constexpr variable ([expr.const], [dcl.constexpr],
// [expr.prim.lambda.closure]).
int g = 1;

int main() {
  constexpr int v = [](int x) { return x + g; }(1); //! error
  return v;
}
