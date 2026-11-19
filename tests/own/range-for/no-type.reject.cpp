// ι (#298): the for-range-declaration is a declaration and needs a decl-specifier-seq with a
// type, so a loop variable named without one is ill-formed ([stmt.pre]: for-range-declaration,
// [dcl.type.general]). The bad loop is the whole body of a function written on one line, so
// that a compiler's recovery from the syntax error stays on that line.
int sum(int (&a)[3]) { int s = 0; for (int x : a) s += x; return s; }

void no_type(int (&a)[3]) { for (x : a) {} } //! error

int main() {
  int a[3] = {1, 2, 3};
  return sum(a) == 6 ? 0 : 1;
}
