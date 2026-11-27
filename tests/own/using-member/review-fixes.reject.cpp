// A using-declared member function conflicts with a data member of the same name in the class
// ([basic.scope.scope]/4, [namespace.udecl]). Clang diagnoses only a use of the name (#1150).
struct B { int f(int) { return 1; } };
struct D : B {
  using B::f; //! error[ycxx]
  int f = 0;  //! error[gcc]
};
int g(D d) { return d.f; } //! error[clang] //! error[gcc]
