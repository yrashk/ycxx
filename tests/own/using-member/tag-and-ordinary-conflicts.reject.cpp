// Ordinary-name conflicts in member using-declarations and invalid qualified elaborated lookups
// ([namespace.udecl], [basic.scope.scope]/4, [basic.lookup.elab], [dcl.type.elab]/2, #1554).
struct B {
  int f() const { return 1; }
};
struct Tag {};

// Ordinary type alias / typedef conflicts (both orders).
struct AliasBefore : B {
  using f = int;
  using B::f;  //! error
};
struct AliasAfter : B { using B::f; using f = int; };  //! error
struct TypedefBefore : B {
  typedef int f;
  using B::f;  //! error
};

// Static data member conflicts (both orders).
struct StaticBefore : B {
  static int f;
  using B::f;  //! error
};
struct StaticAfter : B { using B::f; static int f; };  //! error

// Static data member template conflicts (both orders).
struct StaticTmplBefore : B {
  template<class T> static constexpr int f = 1;
  using B::f;  //! error
};
struct StaticTmplAfter : B { using B::f; template<class T> static constexpr int f = 1; };  //! error

// Member alias template conflicts (both orders).
struct AliasTmplBefore : B {
  template<class T> using f = T;
  using B::f;  //! error
};
struct AliasTmplAfter : B { using B::f; template<class T> using f = T; };  //! error

// Unscoped enumerator conflict.
struct EnumeratorBefore : B {
  enum E { f };
  using B::f;  //! error
};

// Dependent-base template instantiation conflicts.
template<class T> struct DepStatic : T { using T::f; static int f; };  //! error
DepStatic<B> bad_dep_static;

template<class T> struct DepAliasTmpl : T { using T::f; template<class U> using f = U; };  //! error
DepAliasTmpl<B> bad_dep_alias_tmpl;
