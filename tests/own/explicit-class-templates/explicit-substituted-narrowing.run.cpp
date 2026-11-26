// #715 review: an explicit-specifier that narrows or is not constant once substituted is an
// invalid expression, so deduction fails and another candidate is chosen
// ([temp.deduct.general]/8, [dcl.fct.spec]).
// EXIT: 0
struct NC { static bool value; };
bool NC::value = true;
struct K { static constexpr bool value = true; };

struct S {
  int tag;
  template <class U> explicit(sizeof(U)) S(U) : tag(1) {}
  S(double) : tag(2) {}
};

struct T {
  int tag;
  template <class U> explicit(U::value) T(U, int) : tag(1) {}
  T(NC, long) : tag(2) {}
};

int main() {
  S a(1);  // sizeof(int) narrows to bool: S(double)
  if (a.tag != 2) return 1;
  T b(NC{}, 0);  // NC::value is not constant: T(NC, long)
  if (b.tag != 2) return 2;
  T c(K{}, 0);  // explicit(true), but direct-initialization keeps it
  if (c.tag != 1) return 3;
  return 0;
}
