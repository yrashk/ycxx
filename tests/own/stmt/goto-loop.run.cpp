// #337 (ι): loops made of goto alone ([stmt.goto]): a counting loop, a nested pair, and a
// state machine whose states are labels.
// EXIT: 0
int sum_to(int n) {
  int i = 1;
  int sum = 0;
loop:
  if (i > n) goto end;
  sum += i;
  ++i;
  goto loop;
end:
  return sum;
}

int grid(int rows, int cols) {
  int cells = 0;
  int r = 0;
outer:
  if (r == rows) return cells;
  {
    int c = 0;
  inner:
    if (c < cols) {
      ++cells;
      ++c;
      goto inner;
    }
  }
  ++r;
  goto outer;
}

// Counts the 'a's in a 0-terminated string, rejecting at the first 'x'.
int machine(const char* s) {
  int count = 0;
start:
  switch (*s) {
    case 0: goto accept;
    case 'x': goto reject;
    case 'a': ++count;
  }
  ++s;
  goto start;
accept:
  return count;
reject:
  return -1;
}

int main() {
  if (sum_to(10) != 55) return 1;
  if (sum_to(0) != 0) return 2;
  if (grid(3, 4) != 12) return 3;
  if (grid(0, 4) != 0) return 4;
  if (machine("banana") != 3) return 5;
  if (machine("") != 0) return 6;
  if (machine("aaxa") != -1) return 7;
  return 0;
}
