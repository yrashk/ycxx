// δ s1 stretch (#86): struct return values by value, returning a braced list, member access on
// a returned prvalue, nested calls ([stmt.return], [expr.call], [expr.ref]).
// EXIT: 0
struct Vec { int x; int y; };
struct Big { long a[6]; };

Vec add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec scale(Vec a, int k) { Vec r = a; r.x *= k; r.y *= k; return r; }

Big fill(long start) {
  Big b;
  for (int i = 0; i < 6; i++) b.a[i] = start + i;
  return b;
}

int main() {
  Vec p = add({1, 2}, {3, 4});
  if (p.x != 4 || p.y != 6) return 1;
  Vec q = scale(add(p, p), 3);
  if (q.x != 24 || q.y != 36) return 2;
  if (add(q, p).y != 42) return 3;
  Big b = fill(10);
  if (b.a[0] != 10 || b.a[5] != 15) return 4;
  if (fill(100).a[3] != 103) return 5;
  p = add(p, {-4, -6});
  if (p.x != 0 || p.y != 0) return 6;
  return 0;
}
