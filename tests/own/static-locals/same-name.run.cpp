// ι (#291): static block variables with the same name in different blocks of one function, and
// in different functions, are distinct variables ([basic.scope.block]; no linkage,
// [basic.link]), each initialized once and keeping its own state.
// EXIT: 0
int pick(int which) {
  if (which == 0) {
    static int n = 0;
    return ++n;
  }
  if (which == 1) {
    static int n = 100;
    return ++n;
  }
  {
    static int n = 1000;
    {
      static int n = 5000;
      ++n;
    }
    return ++n;
  }
}

int inner_of_pick_is_separate() {
  // Not the 5000 above: a different function's block.
  static int n = 5000;
  return n;
}

int f() {
  static int n = 10;
  return ++n;
}

int g() {
  static int n = 20;
  return ++n;
}

int* addr_f_block() {
  static int n;
  return &n;
}

int* addr_g_block() {
  static int n;
  return &n;
}

int main() {
  if (pick(0) != 1) return 1;
  if (pick(1) != 101) return 2;
  if (pick(2) != 1001) return 3;
  if (pick(0) != 2) return 4;
  if (pick(1) != 102) return 5;
  if (pick(2) != 1002) return 6;
  if (inner_of_pick_is_separate() != 5000) return 7;
  if (f() != 11) return 8;
  if (g() != 21) return 9;
  if (f() != 12) return 10;
  if (g() != 22) return 11;
  if (addr_f_block() == addr_g_block()) return 12;
  *addr_f_block() = 3;
  if (*addr_g_block() != 0) return 13;
  return 0;
}
