// #337 (ι): no two case constants in the same switch may have the same value after conversion
// ([stmt.switch]/2); 1 + 1 and 2 are the same value.
int f(int v) {
  switch (v) {
    case 1: return 10;
    case 2: return 20;
    case 1 + 1: return 30; //! error
  }
  return 0;
}

int main() { return f(1) - 10; }
