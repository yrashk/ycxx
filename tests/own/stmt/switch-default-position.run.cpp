// #337 (ι): the default label may appear anywhere in the switch body; it is taken only when no
// case matches, and control then flows on from its position ([stmt.switch]/5). With no match and
// no default, none of the body is executed.
// EXIT: 0
int first(int v) {
  int r = 0;
  switch (v) {
    default: r += 1;
    case 1: r += 10;
      break;
    case 2: r += 100;
  }
  return r;
}

int middle(int v) {
  int r = 0;
  switch (v) {
    case 1: r += 1;
      break;
    default: r += 10;
    case 2: r += 100;
      break;
    case 3: r += 1000;
  }
  return r;
}

int last(int v) {
  int r = 0;
  switch (v) {
    case 1: r = 1; break;
    case 2: r = 2; break;
    default: r = 3;
  }
  return r;
}

int none(int v) {
  int r = 5;
  switch (v) {
    case 1: r = 1; break;
    case 2: r = 2; break;
  }
  return r;
}

int only_default(int v) {
  int r = 0;
  switch (v) {
    default: r = v * 2;
  }
  return r;
}

int empty_body(int v) {
  switch (v) {}
  return v;
}

int main() {
  if (first(1) != 10) return 1;
  if (first(2) != 100) return 2;
  if (first(9) != 11) return 3;
  if (middle(1) != 1) return 4;
  if (middle(2) != 100) return 5;
  if (middle(3) != 1000) return 6;
  if (middle(9) != 110) return 7;
  if (last(1) != 1) return 8;
  if (last(2) != 2) return 9;
  if (last(0) != 3) return 10;
  if (none(3) != 5) return 11;
  if (none(2) != 2) return 12;
  if (only_default(21) != 42) return 13;
  if (empty_body(4) != 4) return 14;
  return 0;
}
