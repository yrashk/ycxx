// α1 (#39): a simple-declaration needs its ';' ([dcl.pre]). The error points at the end of the
// declaration, where the ';' was expected.
// XFAIL: gcc reports it at the next token, on the next line, not where the ";" was expected
int main() {
  int a = 1 //! error
  return a;
}
