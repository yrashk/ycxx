// θ step 1 (#279), spec-only: `decltype(auto)` must be the whole declared type; a reference declarator is
// ill-formed ([dcl.type.auto.deduct], [dcl.spec.auto.general]).
int main() {
  int v = 1;
  decltype(auto)& x = v; //! error
  return 0;
}
