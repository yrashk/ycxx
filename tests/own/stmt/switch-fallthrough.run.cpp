// #337 (ι): case and default labels do not alter the flow of control; without break, control
// falls through to the following statements ([stmt.switch]/6), and several labels may label
// one statement.
// EXIT: 0
int trace(int v) {
  int r = 0;
  switch (v) {
    case 1: r += 1;
    case 2: r += 10;
    case 3: r += 100;
      break;
    case 4:
    case 5:
    case 6: r += 1000;
    case 7: r += 10000;
  }
  return r;
}

int count_down(int n) {
  int hits = 0;
  switch (n) {
    case 3: ++hits;
    case 2: ++hits;
    case 1: ++hits;
    case 0: break;
  }
  return hits;
}

int main() {
  if (trace(1) != 111) return 1;
  if (trace(2) != 110) return 2;
  if (trace(3) != 100) return 3;
  if (trace(4) != 11000) return 4;
  if (trace(5) != 11000) return 5;
  if (trace(6) != 11000) return 6;
  if (trace(7) != 10000) return 7;
  if (trace(8) != 0) return 8;
  if (count_down(3) != 3) return 9;
  if (count_down(2) != 2) return 10;
  if (count_down(0) != 0) return 11;
  return 0;
}
