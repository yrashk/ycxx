// The access of a member using-declaration is its own ([namespace.udecl]/16): a private or
// protected using-declaration of a public base member makes the name inaccessible through the
// derived class, for member call syntax, operators and further-derived classes. (#1150)
struct B {
  int f(int) { return 1; }
  int operator()(int) const { return 2; }
};
class D : public B {
  using B::f;
protected:
  using B::operator();
};
struct E : D {
  int g() { return f(1); }   //! error
  int h() { return (*this)(1); }
};
int use(D& d) {
  int r = d.f(1);            //! error
  r += d(1);                 //! error
  return r;
}
int main() { return 0; }
