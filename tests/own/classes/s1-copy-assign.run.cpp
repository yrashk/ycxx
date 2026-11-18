// δ s1 (#86): copy initialization from another struct, struct assignment, assignment from a
// braced list, copies of arrays inside structs, chained assignment ([class.copy.ctor],
// [class.copy.assign], [expr.assign]).
// EXIT: 0
struct Pair { int a; int b; };
struct Buf { int len; char text[4]; };

int main() {
  Pair s = {1, 2};
  Pair t = s;
  if (t.a != 1 || t.b != 2) return 1;
  t.a = 10;
  if (s.a != 1) return 2;  // a copy, not an alias
  Pair u{s};
  if (u.b != 2) return 3;
  s = t;
  if (s.a != 10 || s.b != 2) return 4;
  t = {3, 4};
  if (t.a != 3 || t.b != 4) return 5;
  t = {};
  if (t.a != 0 || t.b != 0) return 6;
  Pair x, y;
  x = y = s;
  if (x.a != 10 || y.b != 2) return 7;

  Buf b1 = {3, {'a', 'b', 'c'}};
  Buf b2 = b1;
  b1.text[0] = 'z';
  if (b2.text[0] != 'a' || b2.text[2] != 'c' || b2.text[3] != 0) return 8;
  Buf arr[2] = {b1, b2};
  if (arr[0].text[0] != 'z' || arr[1].text[0] != 'a') return 9;
  arr[0] = arr[1];
  if (arr[0].text[0] != 'a') return 10;
  Pair* p = &x;
  *p = {5, 6};
  if (x.a != 5 || x.b != 6) return 11;
  return 0;
}
