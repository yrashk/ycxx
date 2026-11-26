// A using-declaration of base member functions in an ordinary class: the named functions join the
// derived class's overload set, and a derived member that corresponds to one hides it
// ([namespace.udecl]/3, /11, /12, [class.member.lookup], [over.match.funcs.general]/4). (#1150)
struct B {
  int f(int) { return 1; }
  int f(char) { return 2; }
  int g(int) { return 3; }
  int h(int) { return 4; }
  int k() { return 5; }
};
struct D : B {
  using B::f;
  int f(double) { return 10; }
  using B::g;
  int g(int) { return 11; }          // hides B::g(int)
  int h(long) { return 12; }         // no using-declaration: B::h is hidden by name
  using B::k;
  int k(int) { return 13; }
  // Unqualified calls inside a member function find the introduced functions too.
  int inside() { return f(1) * 1000 + f('c') * 100 + f(1.5) + g(0) + k(); }
};
struct E : D {};                     // through a further-derived class
int main() {
  D d;
  E e;
  int bad = 0;
  if (d.f(1) != 1) bad |= 1;
  if (d.f('c') != 2) bad |= 2;
  if (d.f(1.5) != 10) bad |= 4;
  if (d.g(1) != 11) bad |= 8;
  if (d.h(1) != 12) bad |= 16;
  if (d.k() != 5 || d.k(0) != 13) bad |= 32;
  if (d.inside() != 1000 + 200 + 10 + 11 + 5) bad |= 64;
  if (e.f(1) != 1 || e.f('c') != 2 || e.f(2.5) != 10 || e.g(0) != 11) bad |= 128;
  D* p = &e;
  if (p->f('x') != 2) bad |= 256;
  return bad;
}
