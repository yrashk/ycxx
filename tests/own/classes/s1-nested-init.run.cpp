// δ s1 (#86): nested braces and brace elision in aggregate initialization, arrays of structs,
// arrays inside structs, a two-dimensional array of structs ([dcl.init.aggr]).
// EXIT: 0
struct Point { int x; int y; };
struct Segment { Point from; Point to; };
struct Poly { int n; Point v[3]; };

int main() {
  Segment s = {{1, 2}, {3, 4}};
  if (s.from.x != 1 || s.from.y != 2 || s.to.x != 3 || s.to.y != 4) return 1;
  // brace elision: the inner braces may be left out
  Segment e = {5, 6, 7, 8};
  if (e.from.x != 5 || e.to.y != 8) return 2;
  // a braced inner list with too few elements, then an elided one
  Segment f = {{9}, 10};
  if (f.from.x != 9 || f.from.y != 0 || f.to.x != 10 || f.to.y != 0) return 3;
  Point pts[3] = {{1, 1}, {2, 4}, {3, 9}};
  int sy = 0;
  for (int i = 0; i < 3; i++) sy += pts[i].y;
  if (sy != 14) return 4;
  Point flat[2] = {1, 2, 3};
  if (flat[0].x != 1 || flat[0].y != 2 || flat[1].x != 3 || flat[1].y != 0) return 5;
  Poly tri = {3, {{0, 0}, {4, 0}, {0, 3}}};
  if (tri.n != 3 || tri.v[1].x != 4 || tri.v[2].y != 3) return 6;
  Poly elided = {2, 1, 2, 3, 4};
  if (elided.v[1].x != 3 || elided.v[1].y != 4 || elided.v[2].x != 0) return 7;
  Point grid[2][2] = {{{1, 2}, {3, 4}}, {{5, 6}}};
  if (grid[0][1].y != 4 || grid[1][0].x != 5 || grid[1][1].x != 0) return 8;
  return 0;
}
