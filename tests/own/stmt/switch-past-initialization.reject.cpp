// #337 (ι): a switch jumping to a case label past a declaration with an initializer in the same
// scope is ill-formed ([stmt.dcl]/2, [stmt.switch]/5).
int f(int v) {
  switch (v) {
    case 1:
      int x = 5;
      return x;
    case 2: //! error
      return 2;
  }
  return 0;
}

int main() { return f(3); }
