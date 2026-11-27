// Using-declarations of static member functions and member function templates: they join the
// derived class's overload set, can be called through the class name and the object, and a
// derived template that corresponds (same template-head and signature) hides the base's
// ([namespace.udecl]/1, /11, [over.match.funcs.general]/4, [temp.over]). (#1150)
struct B {
  static int s(int) { return 1; }
  static int s(char) { return 2; }
  template <class T> int t(T) { return 3; }
  template <class T> int u(T, int) { return 4; }
  template <class T> static int st(T*) { return 5; }
  int n(long) { return 6; }
};
struct D : B {
  using B::s;
  static int s(double) { return 11; }
  using B::t;
  int t(int) { return 13; }                        // a non-template beside the template
  using B::u;
  template <class T> int u(T, int) { return 14; }  // corresponds: hides B::u
  using B::st;
  using B::n;
  template <class T> int n(T) { return 16; }       // a template beside a non-template
  static int inside() { return s(1) + s('c') * 10; }
};
int main() {
  D d;
  int x = 0;
  int bad = 0;
  if (D::s(1) != 1 || D::s('c') != 2 || D::s(1.0) != 11) bad |= 1;
  if (d.s(1) != 1 || d.s(0.5) != 11) bad |= 2;
  if (d.t(1) != 13 || d.t('c') != 3 || d.t<int>(1) != 3) bad |= 4;
  if (d.u(1.0, 1) != 14) bad |= 8;
  if (D::st(&x) != 5) bad |= 16;
  if (d.n(1L) != 6 || d.n(1) != 16) bad |= 32;
  if (D::inside() != 21) bad |= 64;
  return bad;
}
