// [gram]: syntax errors in declarations, statements and expressions.
int main() {
  int a = 1 + ; //! error
  int b = (a; //! error
  a = a b; //! error
  if a) {} //! error
  return a;
}
