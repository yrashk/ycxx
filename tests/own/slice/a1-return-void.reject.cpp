// α1 (#39): a return with a value in a void function ([stmt.return]/2).
void f() {
  return 1; //! error
}
int main() { f(); return 0; }
