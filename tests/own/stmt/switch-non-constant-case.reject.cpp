// #337 (ι): a case value shall be a converted constant expression ([stmt.switch]/2,
// [expr.const]); a function parameter is not one.
int f(int v, int w) {
  switch (v) {
    case 1: return 1;
    case w: return 2; //! error
  }
  return 0;
}

int main() { return f(0, 3); }
