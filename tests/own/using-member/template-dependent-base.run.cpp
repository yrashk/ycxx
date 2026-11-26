// Using-declarations naming members of a dependent base: `template<class Fn> struct D : Fn
// { using Fn::operator(); };` names whatever each instantiation's base declares, joins the
// derived class's own overloads, and is hidden by them where they correspond
// ([namespace.udecl]/1, /11, [temp.dep.type], [temp.res.general]). (#1150)
struct Ints {
  int operator()(int) const { return 1; }
  int operator()(int, int) const { return 2; }
  int name(int) { return 3; }
};
struct Chars {
  int operator()(char) const { return 4; }
  int name(char) { return 5; }
};
template <class Fn> struct D : Fn {
  using Fn::operator();
  int operator()(double) const { return 6; }
  using Fn::name;
  int name(long) { return 7; }
  int inside() { return (*this)(1.5) * 10 + name(0L); }
};
template <class Fn> struct Hiding : Fn {
  using Fn::operator();
  int operator()(int) const { return 8; }       // hides Ints::operator()(int)
};
template <class Fn> struct Further : D<Fn> {};
int main() {
  D<Ints> i;
  D<Chars> c;
  Hiding<Ints> h;
  Further<Chars> f;
  int bad = 0;
  if (i(1) != 1 || i(1, 2) != 2 || i(1.0) != 6) bad |= 1;
  if (c('c') != 4 || c(1.0) != 6) bad |= 2;
  if (i.name(1) != 3 || i.name(1L) != 7 || c.name('c') != 5) bad |= 4;
  if (h(1) != 8 || h(1, 2) != 2) bad |= 16;
  if (f('x') != 4 || f(0.5) != 6 || f.name('x') != 5) bad |= 32;
  if (i.inside() != 67 || c.inside() != 67) bad |= 64;
  return bad;
}
