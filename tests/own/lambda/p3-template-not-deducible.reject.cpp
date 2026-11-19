// θ step 3 (#281), spec-only: calling a template lambda whose template parameter appears in no
// function parameter, without explicit template arguments, cannot deduce it, so the call is
// ill-formed ([temp.deduct.call], [temp.deduct.general], [over.call.object]).
int main() {
  auto size = []<class T>() { return sizeof(T); };
  return size(); //! error
}
