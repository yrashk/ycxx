// #337 (ι): a case label shall appear only in a switch statement ([stmt.label]/2); a loop body is
// not one.
int f(int v) {
  int r = 0;
  while (v > 0) {
    case 1: r += v; //! error
    --v;
  }
  return r;
}

int main() { return f(0); }
