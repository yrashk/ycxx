// A using-declaration in a member-specification cannot name a namespace member: its
// nested-name-specifier must name a base class ([namespace.udecl]/3), and it cannot name a
// namespace ([namespace.udecl]/6). (#1150)
namespace N { int f(int); namespace M {} }
int g(int);
struct B { int f(char); };
struct D : B {
  using N::f;   //! error
};
struct E : B {
  using ::g;    //! error
};
struct F : B {
  using N::M;   //! error
};
int main() { return 0; }
