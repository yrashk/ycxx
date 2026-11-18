// [expr.call], [dcl.init]: calls with the wrong number of arguments, arguments that do not convert
// to their parameters, and calls of things that are not functions.
int two(int a, int b) { return a + b; }
void take(int* p) {}
int main() {
  int x = 1;
  two(1); //! error
  two(1, 2, 3); //! error
  take(x); //! error
  x(); //! error
  return two(1, 2);
}
