// Using-declarations naming same-named member functions of two bases put both overload sets in
// the derived class, so lookup of the name finds the derived class's declarations rather than an
// ambiguous merge of the bases' ([namespace.udecl]/1, /12, [class.member.lookup]); overload
// resolution then picks among all of them. (#1150)
struct X {
  int f(int) { return 1; }
  int operator()(int) const { return 3; }
};
struct Y {
  int f(char) { return 2; }
  int operator()(const char*) const { return 4; }
};
struct Z : X, Y {
  using X::f;
  using Y::f;
  using X::operator();
  using Y::operator();
  int f(double) { return 5; }
  int inside() { return f(1) * 100 + f('c') * 10 + f(1.5); }
};
struct W : Z {};
int main() {
  Z z;
  W w;
  int bad = 0;
  if (z.f(1) != 1 || z.f('c') != 2 || z.f(0.5) != 5) bad |= 1;
  if (z(1) != 3 || z("s") != 4) bad |= 2;
  if (z.inside() != 125) bad |= 4;
  if (w.f('c') != 2 || w(1) != 3) bad |= 8;
  return bad;
}
