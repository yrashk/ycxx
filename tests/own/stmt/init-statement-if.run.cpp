// #337 (ι): an if statement with an init-statement ([stmt.if]/4, C++17): a simple-declaration or
// an expression-statement runs once before the condition, and the declared names are in scope
// in the condition and both branches.
// EXIT: 0
int calls = 0;
void f() { ++calls; }
int value() { return 7; }

int main() {
  if (int x = 2; x > 1) {
    if (x != 2) return 1;
  } else {
    return 2;
  }

  if (int x = 2; x > 5) {
    return 3;
  } else {
    if (x != 2) return 4;
  }

  if (f(); calls == 1) {
  } else {
    return 5;
  }
  if (calls != 1) return 6;

  if (int a = 1, b = 2; a + b == 3) {
    if (a != 1 || b != 2) return 7;
  } else {
    return 8;
  }

  // An init-statement together with a condition that is itself a declaration.
  if (int base = 10; int v = value() - base + 3) {
    return 9;
  } else {
    if (v != 0) return 10;
    if (base != 10) return 11;
  }

  // An assignment as the expression-statement.
  int y = 0;
  if (y = 5; y) {
  } else {
    return 12;
  }
  if (y != 5) return 13;

  // if constexpr with an init-statement.
  if constexpr (constexpr int n = 4; n > 3) {
  } else {
    return 14;
  }
  return 0;
}
