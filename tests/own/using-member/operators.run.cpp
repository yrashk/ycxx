// Using-declarations whose unqualified-id is an operator-function-id: `using B::operator();`,
// `using B::operator==;`, `using B::operator[];`, `using B::operator+=;`. The base's operators
// join the derived class's own overloads in operator expressions and explicit calls
// ([namespace.udecl]/1, /11, [over.match.oper], [over.call]). (#1150)
struct B {
  int operator()(int) const { return 1; }
  int operator()(int, int) const { return 2; }
  bool operator==(int v) const { return v == 7; }
  int operator[](int i) const { return 100 + i; }
  int operator+=(int) { return 30; }
};
struct D : B {
  using B::operator();
  int operator()(double) const { return 3; }
  int operator()(int, int) const { return 4; }   // hides B::operator()(int, int) const
  using B::operator==;
  bool operator==(const char*) const { return true; }
  using B::operator[];
  using B::operator+=;
  int operator+=(double) { return 31; }
  int inside() const { return (*this)(1) * 10 + operator()(2, 3); }
};
struct E : D {};
int main() {
  D d;
  E e;
  int bad = 0;
  if (d(1) != 1 || d(1.0) != 3 || d(1, 2) != 4) bad |= 1;
  if (d.operator()(5) != 1) bad |= 2;
  if (!(d == 7) || d == 8 || !(d == "x")) bad |= 4;
  if ((d += 1) != 30 || (d += 1.0) != 31) bad |= 16;
  if (d.inside() != 14) bad |= 32;
  if (e(1) != 1 || e(0.5) != 3) bad |= 64;
  return bad;
}
