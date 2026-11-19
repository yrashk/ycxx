// #337 (ι): a declaration as the condition of a for statement ([stmt.for]/1, [stmt.pre]/4): it
// is initialized before each test, its name is in scope in the body, and a class object there is
// destroyed at the end of each iteration.
// XFAIL: ycxx needs `explicit operator bool()`, a conversion function (#339)
// EXIT: 0
int remaining = 4;
int h() { return remaining--; }

int constructed = 0;
int destroyed = 0;
struct Guard {
  int v;
  Guard(int x) : v(x) { ++constructed; }
  ~Guard() { ++destroyed; }
  explicit operator bool() const { return v > 0; }
};

int main() {
  int sum = 0;
  int steps = 0;
  for (; int x = h(); ++steps) {
    sum += x;
  }
  if (sum != 10) return 1;
  if (steps != 4) return 2;

  int total = 0;
  for (int i = 3; int left = i; --i) {
    total += left * 10;
  }
  if (total != 60) return 3;

  int seen = 0;
  for (int i = 3; Guard g = Guard(i); --i) {
    if (constructed != destroyed + 1) return 4;
    seen += g.v;
  }
  if (seen != 6) return 5;
  if (constructed != 4) return 6;
  if (destroyed != 4) return 7;
  return 0;
}
