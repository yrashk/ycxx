// ι (#298): break, continue and return inside a range-based for: break leaves the loop,
// continue goes to the increment `++__begin` (so the next element follows), and return leaves the
// function from the middle of the loop ([stmt.ranged]/1, [stmt.break], [stmt.cont],
// [stmt.return]); in nested loops each break and continue affects the innermost loop.
// EXIT: 0
int find(const int (&a)[6], int wanted) {
  int i = 0;
  for (int x : a) {
    if (x == wanted) return i;
    ++i;
  }
  return -1;
}

int main() {
  int a[6] = {3, 8, 1, 9, 4, 7};

  int seen = 0;
  for (int x : a) {
    if (x == 9) break;
    ++seen;
  }
  if (seen != 3) return 1;

  int odd_sum = 0;
  int visited = 0;
  for (int x : a) {
    ++visited;
    if (x % 2 == 0) continue;
    odd_sum += x;
  }
  if (odd_sum != 20 || visited != 6) return 2;

  if (find(a, 9) != 3) return 3;
  if (find(a, 3) != 0) return 4;
  if (find(a, 7) != 5) return 5;
  if (find(a, 5) != -1) return 6;

  // break and continue in the inner loop of two.
  int grid[3][3] = {{1, 2, 3}, {4, 0, 6}, {7, 8, 0}};
  int sum = 0;
  int rows = 0;
  for (auto& row : grid) {
    ++rows;
    for (int x : row) {
      if (x == 0) break;
      if (x == 2) continue;
      sum += x;
    }
  }
  if (rows != 3 || sum != 1 + 3 + 4 + 7 + 8) return 7;

  // A break in the inner loop does not stop the outer one; one in the outer one does.
  rows = 0;
  for (auto& row : grid) {
    if (row[0] == 7) break;
    ++rows;
  }
  if (rows != 2) return 8;
  return 0;
}
