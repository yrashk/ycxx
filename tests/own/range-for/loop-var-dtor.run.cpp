// ι (#298): a loop variable of class type with a destructor is declared inside the loop body
// of the equivalent while loop, so it is constructed from `*__begin` and destroyed once per
// iteration, at the end of each iteration, including one left by continue or break
// ([stmt.ranged]/1, [stmt.while], [stmt.jump]/2).
// EXIT: 0
int made = 0;
int destroyed = 0;

struct Tracker {
  int v;
  Tracker(int x) : v(x) { ++made; }
  Tracker(const Tracker& o) : v(o.v) { ++made; }
  ~Tracker() { ++destroyed; }
};

int main() {
  int a[4] = {1, 2, 3, 4};
  int i = 0;
  int sum = 0;
  for (Tracker t : a) {
    // Each earlier iteration's variable is already gone; this one is alive.
    if (made != i + 1) return 1;
    if (destroyed != i) return 2;
    sum += t.v;
    ++i;
  }
  if (sum != 10) return 3;
  if (made != 4 || destroyed != 4) return 4;

  // Copies of class elements: one copy constructed and destroyed per element; the elements
  // themselves are destroyed only when the array goes out of scope.
  made = 0;
  destroyed = 0;
  {
    Tracker ts[3] = {Tracker(5), Tracker(6), Tracker(7)};
    int base_made = made;
    int base_destroyed = destroyed;
    i = 0;
    for (Tracker t : ts) {
      if (made != base_made + i + 1 || destroyed != base_destroyed + i) return 5;
      ++i;
    }
    if (made != base_made + 3 || destroyed != base_destroyed + 3) return 6;

    // By reference: nothing constructed or destroyed.
    for (const Tracker& t : ts) sum += t.v;
    if (made != base_made + 3 || destroyed != base_destroyed + 3) return 7;
  }
  if (made != destroyed) return 8;

  // continue and break still destroy the iteration's variable.
  made = 0;
  destroyed = 0;
  for (Tracker t : a) {
    if (t.v == 2) continue;
    if (t.v == 3) break;
  }
  if (made != 3 || destroyed != 3) return 9;
  return 0;
}
