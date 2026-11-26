// A using-declaration of virtual member functions: the derived declaration that corresponds
// overrides the base's and hides it from the using-declaration, the other overloads stay; a call
// through the introduced name still dispatches virtually ([namespace.udecl]/11, /12,
// [class.virtual]). (#1150)
struct B {
  virtual int f(int) { return 1; }
  virtual int f(char) { return 2; }
  virtual ~B() = default;
};
struct D : B {
  using B::f;
  int f(int) override { return 11; }
};
struct E : D {
  int f(char) override { return 22; }
  using D::f;                        // names D::f(int) and B::f(char), the latter hidden by E's
};
int viaB(B& b) { return b.f(1) * 100 + b.f('c'); }
int main() {
  D d;
  E e;
  int bad = 0;
  if (d.f(1) != 11 || d.f('c') != 2) bad |= 1;
  if (viaB(d) != 1100 + 2) bad |= 2;
  if (e.f(1) != 11 || e.f('c') != 22) bad |= 4;
  if (viaB(e) != 1100 + 22) bad |= 8;
  D& r = e;
  if (r.f('c') != 22) bad |= 16;     // B::f(char) found through D's using-declaration, overridden in E
  return bad;
}
