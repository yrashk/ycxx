// #356: a constructor's ill-formed dependent default argument is instantiated, and diagnosed,
// when a specialization is default-initialized through it ([temp.inst], [dcl.fct.default],
// [class.default.ctor]). The initialization that supplies the argument does not need it.
struct NoMembers {};

template<class T>
struct Holder {
  Holder(int x = T::missing) : v(x) {} //! error
  int v;
};

Holder<NoMembers> supplied(1);
Holder<NoMembers> defaulted;
