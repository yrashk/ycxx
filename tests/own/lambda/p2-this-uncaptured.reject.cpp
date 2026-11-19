// θ step 2 (#280), spec-only: `this` (and a member access through it) in a lambda that neither captures
// `this` nor has a capture-default is ill-formed ([expr.prim.this], [expr.prim.lambda.capture]).
struct S {
  int m;
  int f() {
    auto g = [] { return m; }; //! error
    return g();
  }
};
int main() { S s{1}; return s.f(); }
