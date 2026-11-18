// [conv.ptr], [expr.call], [dcl.fct], [basic.scope.block], [basic.start.main], [expr.mul]: a
// character literal is not a null pointer constant, a void argument has no value, a const
// parameter stays const in the body, a for-init name cannot be redeclared in the loop's body,
// main cannot be used, and a remainder whose quotient overflows is not a constant.
extern "C" int printf(const char*, ...);
void nothing();
int f(const int x) {
  x = 1; //! error
  return x;
}
int* p = '\0'; //! error
int a[(-2147483647 - 1) % -1 + 1]; //! error
int g() {
  printf("%d", nothing()); //! error
  for (int i = 0;;) {
    int i = 1; //! error
  }
  return 0;
}
int h() { return 0; }
