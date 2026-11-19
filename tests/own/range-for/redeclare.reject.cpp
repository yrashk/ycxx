// ι (#298): the loop variable's scope is the statement's, and a name it declares may not be
// redeclared in the outermost block of the body ([stmt.ranged]/1, [basic.scope.block]/2); the
// same holds for a name declared by the init-statement. In a nested block it may be hidden.
int main() {
  int a[3] = {1, 2, 3};
  int s = 0;
  for (int x : a) {
    int x = 1; //! error
    s += x;
  }
  for (int i = 0; int y : a) {
    int i = y; //! error
    s += i;
  }
  for (int x : a) {
    {
      int x = 2;
      s += x;
    }
  }
  return s;
}
