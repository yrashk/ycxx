// [basic.scope.block], [basic.scope.param], [basic.def.odr], [dcl.fct]: redeclarations that are
// not allowed.
int f(int x) {
  int x = 2; //! error
  int y = 1;
  int y = 2; //! error
  return x + y;
}
int g() { return 1; }
int g() { return 2; } //! error
int h();
long h(); //! error
int v = 1;
int v = 2; //! error
int main() { return 0; }
