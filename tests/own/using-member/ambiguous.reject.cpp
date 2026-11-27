// Functions from two using-declarations with the same parameter-type-list do not conflict
// ([namespace.udecl]/10) but make a call ambiguous ([over.match.best]); without using-declarations
// the name's lookup in the derived class is ambiguous ([class.member.lookup]); and a
// using-declaration cannot resolve an ambiguous base ([namespace.udecl]/15). (#1150)
struct X { int f(int); int g(int); };
struct Y { int f(int); int g(char); };
struct Z : X, Y {
  using X::f;
  using Y::f;
};
struct N : X, Y {};
struct A { int x(); };
struct B : A {};
struct C : A {
  using A::x;
  int x(int);
};
struct D : B, C {
  using C::x;
  int x(double);
};
int use(Z& z, N& n, D& d) {
  int r = z.f(1);   //! error
  r += n.g(1);      //! error
  r += d.x();       //! error
  return r;
}
int main() { return 0; }
