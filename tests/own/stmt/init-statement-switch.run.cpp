// #337 (ι): a switch statement with an init-statement ([stmt.switch]/3, C++17): the declared name
// is in scope in the condition and the body.
// EXIT: 0
int counter = 0;
int bump() { return ++counter; }

int classify(int x) {
  switch (int y = x * 3; y) {
    case 3: return y + 1;
    case 6: return y + 2;
    default: return -y;
  }
}

int main() {
  if (classify(1) != 4) return 1;
  if (classify(2) != 8) return 2;
  if (classify(3) != -9) return 3;

  switch (bump(); counter) {
    case 1: break;
    default: return 4;
  }

  switch (int a = 5; int b = a - 3) {
    case 2:
      if (a != 5 || b != 2) return 5;
      break;
    default:
      return 6;
  }
  return 0;
}
