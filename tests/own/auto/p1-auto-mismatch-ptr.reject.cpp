// θ step 1 (#279), spec-only: `auto` replaced by different types in two declarators of one declaration
// (int from the first, long from the pointer declarator) is ill-formed
// ([dcl.spec.auto.general], [dcl.type.auto.deduct]).
int main() {
  int i = 0;
  long l = 0;
  auto a = i, *p = &l; //! error
  return a;
}
