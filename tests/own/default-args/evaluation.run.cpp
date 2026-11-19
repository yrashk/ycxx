// ι (#300): default arguments are evaluated at each call, in the caller, before the function
// body. Only orders the language defines are checked: the operands of `&&`, `,` and `?:`, and
// separate statements ([dcl.fct.default]/9, [expr.call]/5, /7, [expr.log.and], [expr.comma],
// [expr.cond]).
// EXIT: 0
int counter = 0;
int seen_in_body = -1;
int tick() { return ++counter; }

int f(int x = tick()) {
  seen_in_body = counter;  // the default was evaluated before the body
  return x;
}
int read(int x = counter) { return x; }

int main() {
  if (f() != 1 || seen_in_body != 1) return 1;
  if (!(f() == 2 && f() == 3)) return 2;      // left operand first
  int r = (counter = 10, read());             // the comma operator sequences the assignment first
  if (r != 10) return 3;
  r = counter > 5 ? f() : f(0);
  if (r != 11) return 4;
  r = f(99);
  if (r != 99 || counter != 11) return 5;     // not evaluated when the argument is given
  return 0;
}
