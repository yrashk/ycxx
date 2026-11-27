// A member using-declarator whose name the base does not declare is ill-formed: lookup finds
// nothing ([namespace.udecl]/1, [basic.lookup.qual]), for a named function, an operator-function-id
// and a conversion-function-id. (#1150)
struct B {
  int f(int);
  int operator()(int);
  operator int();
};
struct D : B {
  using B::missing;     //! error
  using B::operator[];  //! error
  using B::operator bool; //! error
};
int main() { return 0; }
