// θ step 1 (#279), spec-only: `decltype(auto)` must be the placeholder alone, with no cv-qualifier
// ([dcl.type.auto.deduct], [dcl.spec.auto.general]).
int main() {
  int v = 1;
  const decltype(auto) x = v; //! error
  return 0;
}
