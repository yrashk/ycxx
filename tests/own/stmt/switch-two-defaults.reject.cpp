// #337 (ι): a switch statement has at most one default label ([stmt.switch]/2). Both labels are
// on one line because Clang reports the first and GCC the second.
int f(int v) {
  switch (v) {
    case 3: return 3;
    default: return 1; default: return 2; //! error
  }
}

int main() { return f(3) - 3; }
