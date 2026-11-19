// ι (#298): range-based for over a two-dimensional array (the outer loop variable is a reference
// to a row, itself an array) and over an array of class objects, read by const reference and
// modified through a reference ([stmt.ranged]/1, [dcl.array]).
// EXIT: 0
struct Point {
  int x;
  int y;
};

int main() {
  int grid[2][3] = {{1, 2, 3}, {4, 5, 6}};
  int sum = 0;
  int rows = 0;
  for (auto& row : grid) {
    ++rows;
    for (int x : row) sum += x;
  }
  if (rows != 2 || sum != 21) return 1;

  for (int (&row)[3] : grid)
    for (int& x : row) x *= 10;
  if (grid[0][0] != 10 || grid[1][2] != 60) return 2;

  sum = 0;
  for (const auto& row : grid)
    for (const auto& x : row) sum += x;
  if (sum != 210) return 3;

  Point pts[3] = {{1, 2}, {3, 4}, {5, 6}};
  int sx = 0;
  int sy = 0;
  for (const Point& p : pts) {
    sx += p.x;
    sy += p.y;
  }
  if (sx != 9 || sy != 12) return 4;

  for (Point& p : pts) p.x = p.y;
  if (pts[0].x != 2 || pts[2].x != 6) return 5;

  // By value: each p is a copy.
  for (Point p : pts) p.y = 0;
  if (pts[1].y != 4) return 6;
  return 0;
}
