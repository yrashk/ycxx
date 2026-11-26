// Every declaration a member using-declarator names must be accessible ([namespace.udecl]/14):
// a private member of the base and an overload set with a private member are rejected at the
// using-declaration. (#1150)
class A {
  int priv(int);
  void f(char);
public:
  void f(int);
  int pub();
};
struct D : A {
  using A::priv;    //! error
};
struct E : A {
  using A::f;       //! error
};
int main() { return 0; }
