// θ step 1 (#279), spec-only: every declarator of one `auto` declaration must deduce the same type
// ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
int main() {
  auto x = 1, y = 'c'; //! error
  return x;
}
