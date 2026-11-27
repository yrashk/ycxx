// A declaration named by two using-declarators in the same class scope makes the program
// ill-formed ([namespace.udecl]/8): the same member function twice, and an operator twice.
// (#1150)
struct A { int f(int); int operator()(int); };
struct D : A {
  using A::f;
  using A::f;           //! error
};
struct E : A {
  using A::operator();
  using A::operator();  //! error
};
int main() { return 0; }
