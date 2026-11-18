// [dcl.pre], [dcl.constexpr], [expr.const]: a static_assert whose condition is false is
// ill-formed, at namespace and block scope; so is a constexpr variable whose initializer is not
// a constant expression.
constexpr int three() { return 3; }
constexpr int zero() { return 0; }
static_assert(three() == 3);
static_assert(three() == 4); //! error
static_assert(false, "always fails"); //! error
static_assert(1 + 1 == 3, "arithmetic"); //! error
static_assert(three() == 3, "holds");
constexpr int ok = 10 / three();
constexpr int bad = 10 / zero(); //! error
int runtime = 2;
constexpr int also_bad = runtime + 1; //! error
const int not_constexpr_but_const = 2;
constexpr int fine = not_constexpr_but_const * 2;
static_assert(fine == 4 && ok == 3);
void block(int param) {
  static_assert(three() == 3);
  static_assert(three() < 0); //! error
  constexpr int local = three() + 1;
  static_assert(local == 4);
  constexpr int from_param = param; //! error
  constexpr int div0 = local / zero(); //! error
  int mutable_local = 1;
  constexpr int from_local = mutable_local; //! error
}
