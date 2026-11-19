// #119: members of a class defined with `class` are private by default, with `struct` public;
// an access-specifier applies to the members after it ([class.access]/2, [class.access.spec]).
class C { int a; public: int b; private: int c; };
struct S { int a; private: int b; public: int c; };
int f(C& x, S& y) {
  int r = x.a;  //! error
  r += x.b;
  r += x.c;     //! error
  r += y.a;
  r += y.b;     //! error
  r += y.c;
  return r;
}
int main() { return 0; }
