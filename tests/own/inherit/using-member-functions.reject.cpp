// A member using-declarator names a member of a base class ([namespace.udecl]/3), one that
// exists and is accessible (/14), and is not repeated in its class ([class.mem.general]) (#1150).
namespace N { int f(); }
struct B {
  int f(int) { return 1; }
private:
  int p();
};
struct U {};
struct C1 : B { using U::f; }; //! error
struct C2 : B { using N::f; }; //! error
struct C3 : B { using B::nope; }; //! error
struct C4 : B { using B::p; }; //! error
struct C5 : B {
  using B::f;
  using B::f; //! error
};
template<class T> struct C6 : T { using T::missing; }; //! error
C6<B> c6;
