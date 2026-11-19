// ι (#298): a range expression that is a prvalue of class type: the temporary is bound to
// `auto&& __range` and so lives until the loop ends ([stmt.ranged]/1, [class.temporary]/6); it
// is destroyed exactly once, after the last iteration and before the statement after the loop.
// Also when the loop is left by break.
// EXIT: 0
int alive = 0;
int destroyed = 0;

struct Range {
  int v[3];
  Range(int base) {
    v[0] = base;
    v[1] = base + 1;
    v[2] = base + 2;
    ++alive;
  }
  ~Range() {
    --alive;
    ++destroyed;
  }
  int* begin() { return v; }
  int* end() { return v + 3; }
};

Range make(int base) { return Range(base); }

int main() {
  int sum = 0;
  int iterations = 0;
  for (int x : make(10)) {
    if (alive != 1 || destroyed != 0) return 1;
    sum += x;
    ++iterations;
  }
  if (iterations != 3 || sum != 33) return 2;
  if (alive != 0 || destroyed != 1) return 3;

  // Elements of the temporary read by reference stay valid in the body.
  sum = 0;
  for (const int& x : make(1)) {
    if (destroyed != 1) return 4;
    sum += x;
  }
  if (sum != 6 || destroyed != 2) return 5;

  for (int x : make(5)) {
    if (x == 6) break;
  }
  if (alive != 0 || destroyed != 3) return 6;
  return 0;
}
