// #337 (ι): a condition that is a declaration ([stmt.pre]/4-/6, [stmt.if]): the declared name is
// in scope in both branches (and in an else-if), its value is contextually converted to bool, and
// a pointer condition is true when not null.
// EXIT: 0
int calls = 0;
int f(int v) { ++calls; return v; }

int table[4] = {3, 5, 7, 9};
int* find(int key) {
  for (int i = 0; i < 4; ++i)
    if (table[i] == key) return &table[i];
  return nullptr;
}

int both_branches(int v) {
  if (int x = f(v)) {
    return x * 10;
  } else {
    return x - 1;
  }
}

int chain(int v, int w) {
  if (int a = v) {
    return 1;
  } else if (int b = w) {
    return b + a;
  } else {
    return 100 + a + b;
  }
}

int main() {
  if (both_branches(4) != 40) return 1;
  if (both_branches(0) != -1) return 2;
  if (calls != 2) return 3;
  if (chain(3, 0) != 1) return 4;
  if (chain(0, 5) != 5) return 5;
  if (chain(0, 0) != 100) return 6;
  if (int* p = find(7)) {
    if (*p != 7) return 7;
    *p = 70;
  } else {
    return 8;
  }
  if (table[2] != 70) return 9;
  if (int* p = find(4)) {
    return 10;
  } else {
    if (p != nullptr) return 11;
  }
  if (bool b = calls == 2) {
    if (!b) return 12;
  } else {
    return 13;
  }
  if (char c = 'q') {
    if (c != 'q') return 14;
  } else {
    return 15;
  }
  if (int braced{0}) {
    return 16;
  } else {
    if (braced != 0) return 17;
  }
  return 0;
}
