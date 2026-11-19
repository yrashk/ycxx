// #337 (ι): a default label shall appear only in a switch statement ([stmt.label]/2).
int f(int v) {
  if (v) {
  default: //! error
    return 1;
  }
  return 0;
}

int main() { return f(0); }
