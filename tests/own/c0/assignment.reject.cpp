// [expr.assign], [expr.pre.incr], [basic.lval]: the left operand of an assignment and the operand
// of ++ and -- must be modifiable lvalues; ++ and -- do not apply to bool.
const int limit = 3;
int main() {
  int a = 1;
  const int c = 2;
  bool b = false;
  3 = a; //! error
  c = 4; //! error
  limit += 1; //! error
  a + 1 = 2; //! error
  ++b; //! error
  b--; //! error
  (a = 1) = 2;
  "text" = 0; //! error
  return a;
}
