// δ s1 (#86): struct definitions, `.` member access, nested members, reading and writing
// members, assigning a member of struct type ([class.mem], [expr.ref]).
// EXIT: 0
struct Point {
  int x;
  int y;
};

struct Rect {
  Point lo;
  Point hi;
};

int main() {
  Point p;
  p.x = 3;
  p.y = 4;
  if (p.x != 3) return 1;
  if (p.y != 4) return 2;
  p.x += p.y;
  if (p.x != 7) return 3;

  Rect r;
  r.lo.x = 1;
  r.lo.y = 2;
  r.hi.x = 11;
  r.hi.y = 7;
  int area = (r.hi.x - r.lo.x) * (r.hi.y - r.lo.y);
  if (area != 50) return 4;

  r.lo = p;
  if (r.lo.x != 7 || r.lo.y != 4) return 5;
  ++r.hi.y;
  r.hi.x--;
  if (r.hi.x != 10 || r.hi.y != 8) return 6;
  return 0;
}
