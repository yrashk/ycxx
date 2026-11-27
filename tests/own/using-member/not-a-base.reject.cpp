// A member using-declarator's nested-name-specifier must name a base class of the current class
// ([namespace.udecl]/3); a class that is not a base, and the class itself, are rejected. (#1150)
struct B { int f(int); };
struct C { int f(char); int g(); };
struct D : B {
  using B::f;
  using C::f;   //! error
};
struct E : B {
  using C::g;   //! error
};
struct F {
  int h();
  using F::h;   //! error
};
int main() { return 0; }
