// A using-declaration has the access of its own member-declaration, not the named member's
// ([namespace.udecl]/16, [class.access.base]): a public using-declaration of a protected base
// member, of a member of a private or protected base, and of an indirect base's member makes it
// callable from outside; a protected one is usable in a further-derived class. (#1150)
class A {
protected:
  int p(int) { return 1; }
  int p(char) { return 2; }
  static int sp() { return 3; }
public:
  int q() { return 4; }
  int operator()(int) { return 5; }
};
struct PubOfProtected : A {
  using A::p;
  using A::sp;
};
class FromPrivate : A {
public:
  using A::q;
  using A::operator();
};
class FromProtectedBase : protected A {
protected:
  using A::q;
};
struct Further : FromProtectedBase {
  int r() { return q() * 10; }       // the protected using-declaration is accessible here
};
struct Mid : A {};
struct Indirect : Mid {
  using A::p;                        // names a member of an indirect base
};
int main() {
  PubOfProtected a;
  FromPrivate b;
  Further c;
  Indirect d;
  int bad = 0;
  if (a.p(1) != 1 || a.p('c') != 2) bad |= 1;
  if (PubOfProtected::sp() != 3) bad |= 2;
  if (b.q() != 4 || b(0) != 5) bad |= 4;
  if (c.r() != 40) bad |= 8;
  if (d.p('x') != 2 || d.p(0) != 1) bad |= 16;
  return bad;
}
