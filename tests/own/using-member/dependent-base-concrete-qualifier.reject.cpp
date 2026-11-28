// Invalid specializations and ordinary non-base member using-declarations are still diagnosed
// ([namespace.udecl]/3, [namespace.udecl]/8, [temp.inst], #1553).
struct B {
  int f() { return 1; }
private:
  int priv() { return 2; }
};
using AliasB = B;
using AliasInt = int;

struct Other {
  int g() { return 3; }
};

namespace N {
  int f();
}

// Valid specialization in the same TU must not suppress invalid specialization diagnostics.
template<class T> struct D : T {
  using B::f;                        //! error
};
D<B> ok_inst;
D<Other> bad_inst;

// Non-dependent base lists still diagnose immediately without instantiation.
template<class T> struct OnlyConcrete : Other {
  using B::f;                        //! error
};

template<class T> struct NoBase {
  using B::f;                        //! error
};

template<class T> struct Outer : T {
  struct Inner : Other {
    using B::f;                      //! error
  };
};

// Non-class and namespace qualifiers still diagnose immediately even with a dependent base.
template<class T> struct NonClass : T {
  using AliasInt::f;                 //! error
};

template<class T> struct NsQual : T {
  using N::f;                        //! error
};

// Duplicate concrete qualifiers in a dependent-base template diagnose in the template definition.
template<class T> struct DupList : T {
  using B::f, B::f;                  //! error
};

template<class T> struct DupAlias : T {
  using B::f, AliasB::f;             //! error
};

template<class T> struct DupSep : T {
  using B::f;
  using B::f;                        //! error
};

// Missing or private members of B diagnose when instantiated.
template<class T> struct Missing : T {
  using B::missing;                  //! error
};
Missing<B> bad_missing;

template<class T> struct Priv : T {
  using B::priv;                     //! error
};
Priv<B> bad_priv;

// A nested class's own non-dependent base hides an enclosing deferred member using-declaration,
// so an inaccessible member in that base must be diagnosed rather than falling through.
struct PrivSub {
private:
  static constexpr int f() { return 7; }
};

template<class T> struct OuterWithPrivSub : T {
  using B::f;
  struct Inner : PrivSub {
    static constexpr int x = f();    //! error
  };
};
int bad_priv_sub = OuterWithPrivSub<B>::Inner::x;
