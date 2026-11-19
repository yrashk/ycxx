// ι (#291): the initializer of a static block variable may use the function's parameters and
// earlier automatic variables; it is evaluated once, on the first pass, so the variable keeps
// the first call's values ([stmt.dcl]/3).
// EXIT: 0
int remember(int x) {
  static int first = x;
  return first;
}

int derived(int x, int y) {
  int sum = x + y;
  int twice = sum * 2;
  static int s = twice + 1;
  static int p = x * 100 + s;
  return p;
}

const int* where(int* out_of_first) {
  static const int* p = out_of_first;
  return p;
}

int main() {
  if (remember(7) != 7) return 1;
  if (remember(9) != 7) return 2;
  if (remember(-3) != 7) return 3;
  // s = (2 + 3) * 2 + 1 = 11; p = 200 + 11.
  if (derived(2, 3) != 211) return 4;
  if (derived(50, 60) != 211) return 5;
  int a = 1;
  int b = 2;
  if (where(&a) != &a) return 6;
  if (where(&b) != &a) return 7;
  return 0;
}
