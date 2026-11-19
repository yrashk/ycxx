// #337 (ι): a case value is converted to the promoted type of the condition as a converted
// constant expression, so a narrowing conversion is ill-formed ([stmt.switch]/2,
// [expr.const], [dcl.init.list]): -1 does not fit unsigned.
int f(unsigned u) {
  switch (u) {
    case 1: return 1;
    case -1: return 2; //! error
  }
  return 0;
}

int main() { return f(0); }
