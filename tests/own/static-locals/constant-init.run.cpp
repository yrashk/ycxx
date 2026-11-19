// ι (#291): static block variables with constant initializers are constant-initialized as part
// of static initialization ([basic.start.static]/2, /3), so they hold their values even when
// control has never passed through their declarations. A goto may jump past a static block
// variable's declaration ([stmt.dcl]/2 forbids that only for automatic variables). A static
// block variable without an initializer is zero-initialized ([basic.start.static]/2).
// EXIT: 0
int sum(bool skip) {
  if (skip) goto use;
  return -1;
  static int a[3] = {1, 2, 3};
  static const int k = 42;
  static constexpr int c = 7 * 6;
  static int z;
  static int zs[4];
use:
  return a[0] + a[1] + a[2] + k + c + z + zs[0] + zs[3];
}

struct P {
  int x;
  int y;
};

int* bump(bool skip) {
  if (skip) goto out;
  {
  }
  static int counter = 10;
  ++counter;
out:
  return &counter;
}

const P* point() {
  static const P p = {3, 4};
  return &p;
}

int main() {
  // The declarations in sum() are never reached; the values are already there.
  if (sum(true) != 1 + 2 + 3 + 42 + 42) return 1;
  if (*bump(true) != 10) return 2;
  if (*bump(false) != 11) return 3;
  if (*bump(true) != 11) return 4;
  if (bump(true) != bump(true)) return 5;
  if (*bump(false) != 12) return 6;
  if (point()->x != 3 || point()->y != 4) return 7;
  if (point() != point()) return 8;
  return 0;
}
