// θ step 1 (#279), spec-only: `auto` deduced from a void expression would declare a variable of type void
// ([dcl.type.auto.deduct], [basic.types.general], [dcl.init]).
void nothing() {}
int main() {
  auto x = nothing(); //! error
  return 0;
}
