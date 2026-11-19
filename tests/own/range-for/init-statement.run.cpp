// ι (#298): C++20's init-statement in a range-based for: a simple-declaration of an array used
// as the range, an object of class type made by a function and iterated by reference, an
// expression-statement; names declared by the init-statement are in scope in the range
// expression and the body, and only there
// ([stmt.ranged]/1, [stmt.pre], [basic.scope.block]).
// EXIT: 0
struct Bag {
  int v[3];
  int* begin() { return v; }
  int* end() { return v + 3; }
};

int bags_made = 0;
Bag make() {
  ++bags_made;
  Bag b;
  b.v[0] = 4;
  b.v[1] = 5;
  b.v[2] = 6;
  return b;
}

int main() {
  int s = 0;
  for (int a[3] = {1, 2, 3}; int x : a) s += x;
  if (s != 6) return 1;

  // The init-statement's object is modified through the loop variable and observed in the body.
  s = 0;
  for (Bag t = make(); auto& x : t) {
    x *= 2;
    s += t.v[0];
  }
  if (s != 24) return 2;
  if (bags_made != 1) return 3;

  // An expression-statement.
  int n = 0;
  int a[4] = {1, 1, 1, 1};
  for (n = 10; int x : a) n += x;
  if (n != 14) return 4;

  // A name from the init-statement used in the range expression.
  int grid[2][2] = {{1, 2}, {3, 4}};
  s = 0;
  for (int row = 1; int x : grid[row]) s += x;
  if (s != 7) return 5;

  // The init-statement runs once, before the range expression.
  int order = 0;
  for (order = 1; int x : a) {
    if (order != 1) return 6;
    s += x;
  }

  // An init name shadows an outer one only for the loop.
  int x0 = 100;
  for (int x0 = 0; int x : a) x0 += x;
  if (x0 != 100) return 7;
  return 0;
}
