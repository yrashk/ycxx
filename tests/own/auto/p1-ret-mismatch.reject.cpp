// θ step 1 (#279), spec-only: the return statements of a function with a deduced return type must deduce
// the same type ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
auto f(bool b) {
  if (b) return 1;
  return 'c'; //! error
}
int main() { return f(true); }
