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

// A member operator is the best candidate, so its operand conversion's failure is the error, not
// a reason to use the worse non-member ([over.match.oper]).
struct W {
  W(int x) : v(x) {}
  int v;
};
struct Y {
  int operator+(Z) const { return 1; }
};
int operator+(const volatile Y&, W) { return 2; }
int member_operator(const Y& y, int n) { return y + n; } //! error
