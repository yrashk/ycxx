// #337 (ι): since C++23 a label may end a compound statement, with no statement after it
// ([stmt.label]/1, [stmt.block], P2324), and several labels may end one.
// A case or default label ending a switch body is left out: GCC 16 rejects it (#337 report).
// EXIT: 0
int skip_rest(int v) {
  int r = 0;
  {
    if (v) goto end;
    r = 10;
  end:
  }
  return r;
}

int in_loop() {
  int odd = 0;
  for (int i = 0; i < 10; ++i) {
    if (i % 2 == 0) goto next;
    ++odd;
  next:
  }
  return odd;
}

int several(int v) {
  int r = 0;
  {
    if (v == 1) goto one;
    if (v == 2) goto two;
    r = 3;
  one:
  two:
  }
  return r;
}

int main() {
  if (skip_rest(1) != 0) return 1;
  if (skip_rest(0) != 10) return 2;
  if (in_loop() != 5) return 3;
  if (several(1) != 0) return 4;
  if (several(2) != 0) return 5;
  if (several(3) != 3) return 6;
  return 0;
}
