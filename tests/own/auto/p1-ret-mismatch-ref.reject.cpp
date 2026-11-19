// θ step 1 (#279), spec-only: `decltype(auto)` return statements deducing int& and int differ
// ([dcl.type.auto.deduct], [dcl.type.decltype], [dcl.spec.auto.general]).
int g = 0;
decltype(auto) f(bool b) {
  if (b) return (g);
  return g; //! error
}
int main() { return f(true); }
