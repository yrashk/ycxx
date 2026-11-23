// #102, [dcl.constexpr], [expr.const]: an immediate invocation of a consteval constructor must be
// a constant expression, wherever the constructor is selected.
struct Z {
  consteval Z(int v) : value(v) {}
  int value;
};
int from_parameter(int n) {
  Z z(n); //! error
  return z.value;
}
int from_cast(int n) { return Z(n).value; } //! error
int from_list(int n) { return Z{n}.value; } //! error
int global = 3;
Z from_global(global); //! error
constexpr int not_immediate(int n) { return Z(n).value; } //! error
