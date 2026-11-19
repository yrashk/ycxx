// #337 (ι): a name declared in an init-statement shall not be redeclared in the outermost block
// of the substatement, the else branch included ([basic.scope.block]/2, [stmt.if]).
int f() {
  if (int y = 3; y > 5) {
    return 1;
  } else {
    int y = 4; //! error
    return y;
  }
}

int main() { return f() - 4; }
