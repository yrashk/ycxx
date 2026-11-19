// θ step 1 (#279), spec-only: a variable declared with a placeholder type needs an initializer to deduce
// from ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
int main() {
  auto x; //! error
  return 0;
}
