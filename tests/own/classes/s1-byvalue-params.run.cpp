// δ s1 stretch (#86): struct parameters by value are copies; changing the parameter does not
// change the argument; a braced list as an argument ([expr.call], [dcl.init.list]).
// EXIT: 0
struct Vec { int x; int y; };
struct Big { int a[5]; int n; };

int dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }

int clobber(Big b) {
  int s = 0;
  for (int i = 0; i < b.n; i++) { s += b.a[i]; b.a[i] = -1; }
  return s;
}

int main() {
  Vec u = {2, 3};
  Vec v{4, 5};
  if (dot(u, v) != 23) return 1;
  if (dot({1, 0}, v) != 4) return 2;
  Big b = {{1, 2, 3, 4, 5}, 5};
  if (clobber(b) != 15) return 3;
  if (b.a[0] != 1 || b.a[4] != 5) return 4;
  return 0;
}
