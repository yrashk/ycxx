// #337 (ι): a case or default label is associated with the innermost enclosing switch
// ([stmt.switch]/2); break in an inner switch leaves only that switch.
// EXIT: 0
int classify(int a, int b) {
  int r = 0;
  switch (a) {
    case 1:
      switch (b) {
        case 1: r = 11; break;
        case 2: r = 12; break;
        default: r = 10;
      }
      r += 100;
      break;
    case 2:
      switch (b) {
        case 1: r = 21;
        case 2: r += 2;
      }
      break;
    default:
      switch (b) {
        default: r = 99;
      }
  }
  return r;
}

int main() {
  if (classify(1, 1) != 111) return 1;
  if (classify(1, 2) != 112) return 2;
  if (classify(1, 7) != 110) return 3;
  if (classify(2, 1) != 23) return 4;
  if (classify(2, 2) != 2) return 5;
  if (classify(2, 3) != 0) return 6;
  if (classify(5, 5) != 99) return 7;
  return 0;
}
