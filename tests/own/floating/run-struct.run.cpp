// [class.mem], [dcl.init.aggr], [expr.call]: structs with double and float members, passed and
// returned by value and through pointers within one program.
// STDOUT: 4 6
// STDOUT: 2.5 -1 0.25
// STDOUT: 3 7.5 1
// EXIT: 0
#include <stdio.h>

struct Vec { double x, y; };
struct Mixed { float f; int i; double d; };
struct Five { double a, b, c, d, e; };

Vec add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
void scale(Vec* v, double k) { v->x *= k; v->y *= k; }
Mixed make(float f, int i, double d) { return {f, i, d}; }
Five fill(double base) { return {base, base * 2, base * 3, base * 4, base * 5}; }
double total(Five f) { return f.a + f.b + f.c + f.d + f.e; }

int main() {
  Vec v = add({1.5, 2.5}, {0.5, 0.5});
  scale(&v, 2);
  printf("%g %g\n", v.x, v.y);
  Mixed m = make(2.5f, -1, 0.25);
  printf("%g %d %g\n", m.f, m.i, m.d);
  Five f = fill(0.5);
  printf("%g %g %g\n", f.e + f.a, total(f), f.b);
  return 0;
}
