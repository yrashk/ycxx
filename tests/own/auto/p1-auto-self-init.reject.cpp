// θ step 1 (#279), spec-only: a variable with an undeduced placeholder type cannot appear in its own
// initializer ([dcl.spec.auto.general]).
int main() {
  auto x = x; //! error
  return 0;
}
