// #133: a member operator= is called for `a = b` ([over.assign]); non-member == and != with
// class operands ([over.match.oper]); a class without operator= keeps the implicit copy.
// EXIT: 0
int count = 0;
struct S { int v; S& operator=(const S& o) { v = o.v + 1; ++count; return *this; } };
struct P { int v; };
struct V { int x, y; };
bool operator==(const V& a, const V& b) { return a.x == b.x && a.y == b.y; }
bool operator!=(const V& a, const V& b) { return !(a == b); }
int main() {
  S a{1}, b{5};
  a = b;
  P p{1}, q{2};
  p = q;
  V v{1, 2}, w{1, 2};
  return a.v - 6 + count - 1 + p.v - 2 + (v == w ? 0 : 1) + (v != w ? 1 : 0);
}
