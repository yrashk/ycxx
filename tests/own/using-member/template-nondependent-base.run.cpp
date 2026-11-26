// Using-declarations of member functions in a class template whose base is not dependent: the
// using-declarator is bound when the template is defined, and each specialization's overload set
// has the base's functions beside its own ([namespace.udecl]/3, /11, [temp.res],
// [temp.dep.type]). (#1150)
struct B {
  int f(int) { return 1; }
  int f(char) { return 2; }
  int operator()(int) const { return 3; }
  static int s() { return 4; }
};
template <class T> struct D : B {
  using B::f;
  int f(T) { return 10; }
  using B::operator();
  int operator()(T, T) const { return 20; }
  using B::s;
  int inside() { return f(1) * 100 + f('c') + s(); }
};
int main() {
  D<double> a;
  D<int> b;                          // D<int>::f(int) hides B::f(int)
  int bad = 0;
  if (a.f(1) != 1 || a.f('c') != 2 || a.f(1.5) != 10) bad |= 1;
  if (b.f(1) != 10 || b.f('c') != 2) bad |= 2;
  if (a(1) != 3 || a(1.0, 2.0) != 20) bad |= 4;
  if (D<long>::s() != 4) bad |= 16;
  if (a.inside() != 100 + 2 + 4 || b.inside() != 1000 + 2 + 4) bad |= 32;
  return bad;
}
