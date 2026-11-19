// ι (#313): an overloaded function name without a target type has no address: `auto` deduces
// from no single function ([over.over], [dcl.type.auto.deduct]).
int f(int) { return 1; }
int f(long) { return 2; }

int main() {
  auto p = f; //! error
  auto q = &f; //! error
  return 0;
}
