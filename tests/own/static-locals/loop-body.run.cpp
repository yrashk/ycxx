// ι (#291): a static block variable in a loop body is initialized once, on the first
// iteration, and keeps its value across iterations and across calls ([stmt.dcl]/3); an
// automatic one beside it is initialized on every iteration.
// EXIT: 0
int inits = 0;

int next(int start) {
  ++inits;
  return start;
}

int run(int times) {
  int sum = 0;
  for (int i = 0; i < times; i++) {
    static int s = next(100);
    int a = next(0);
    s += 1;
    a += 1;
    sum += s + a;
  }
  return sum;
}

int whiles() {
  int k = 0;
  int total = 0;
  while (k < 4) {
    static int seen = 0;
    seen += k;
    total = seen;
    ++k;
  }
  return total;
}

int main() {
  // s: 101, 102, 103; a: 1 each.
  if (run(3) != 101 + 102 + 103 + 3) return 1;
  if (inits != 1 + 3) return 2;
  // The static carries over into the second call: 104, 105.
  if (run(2) != 104 + 105 + 2) return 3;
  if (inits != 1 + 3 + 2) return 4;
  if (whiles() != 0 + 1 + 2 + 3) return 5;
  if (whiles() != 12) return 6;
  return 0;
}
