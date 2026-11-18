// α1 (#39): an undeclared name in an expression ([basic.lookup.unqual]).
int main() {
  int a = 1;
  return a + b; //! error
}
