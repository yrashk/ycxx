// Local classes (#117), spec-only: two functions each define a local class named Item; each name is
// scoped to its own block, so the two are distinct classes with different members
// ([class.local], [basic.scope.block]).
int one() {
  struct Item {
    int a;
    int get() const { return a + 1; }
  };
  Item i{10};
  return i.get();
}

int two() {
  struct Item {
    long a[4];
    int get() const { return static_cast<int>(a[0] + a[3]) + 1000; }
  };
  Item i{{1, 2, 3, 4}};
  if (sizeof(Item) != 4 * sizeof(long)) return -1;
  return i.get();
}

int main() {
  if (one() != 11) return 1;
  if (two() != 1005) return 2;
  if (one() + two() != 1016) return 3;
  return 0;
}
