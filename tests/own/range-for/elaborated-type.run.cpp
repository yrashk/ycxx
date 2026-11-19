// ι (#298): an elaborated-type-specifier can name the element type in a for-range-declaration,
// with or without an init-statement ([stmt.pre], [stmt.ranged]/1, [dcl.type.elab]).
// EXIT: 0
struct Point {
  int x;
};

enum Step { first = 4, second = 7 };

int main() {
  Point points[2] = {{2}, {3}};
  Step steps[2] = {first, second};
  int sum = 0;
  for (struct Point p : points) sum += p.x;
  for (enum Step s : steps) sum += s;
  if (sum != 16) return 1;

  sum = 0;
  for (int scale = 2; struct Point p : points) sum += scale * p.x;
  for (int scale = 3; enum Step s : steps) sum += scale * s;
  return sum == 43 ? 0 : 2;
}
