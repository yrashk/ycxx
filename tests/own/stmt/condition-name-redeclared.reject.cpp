// #337 (ι): a name declared in a condition shall not be redeclared in the outermost block of the
// substatement ([basic.scope.block]/2, [stmt.pre]).
int f() {
  if (int x = 1) {
    int x = 2; //! error
    return x;
  }
  return 0;
}

int main() { return f() - 2; }
