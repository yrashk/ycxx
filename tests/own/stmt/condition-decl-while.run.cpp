// #337 (ι): a declaration in a while condition is initialized on every iteration and destroyed
// at the end of each iteration, before the next test ([stmt.while]/2, [stmt.pre]/5); break and
// continue destroy it too.
// XFAIL: ycxx needs `explicit operator bool()`, a conversion function (#339)
// EXIT: 0
int remaining = 5;
int next() { return remaining--; }

int constructed = 0;
int destroyed = 0;
int live = 0;
int max_live = 0;

struct Token {
  int v;
  Token(int x) : v(x) {
    ++constructed;
    ++live;
    if (live > max_live) max_live = live;
  }
  ~Token() {
    ++destroyed;
    --live;
  }
  explicit operator bool() const { return v != 0; }
};

int source = 3;
int take() { return source--; }

int main() {
  int sum = 0;
  int iterations = 0;
  while (int n = next()) {
    sum += n;
    ++iterations;
  }
  if (sum != 15) return 1;
  if (iterations != 5) return 2;

  int seen = 0;
  while (Token t = Token(take())) {
    if (live != 1) return 3;
    if (constructed != destroyed + 1) return 4;
    seen += t.v;
  }
  // Three true tokens and the final false one were created; each was destroyed.
  if (seen != 6) return 5;
  if (constructed != 4) return 6;
  if (destroyed != 4) return 7;
  if (max_live != 1) return 8;

  source = 5;
  constructed = destroyed = 0;
  int visited = 0;
  while (Token t = Token(take())) {
    if (t.v == 4) continue;
    if (t.v == 2) break;
    ++visited;
  }
  if (visited != 2) return 9;
  if (constructed != 4) return 10;
  if (destroyed != 4) return 11;
  if (live != 0) return 12;
  return 0;
}
