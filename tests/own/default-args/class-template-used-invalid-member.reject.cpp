// #356: a member function's ill-formed dependent default argument is instantiated, and
// diagnosed, when a call of a specialization uses it ([temp.inst], [dcl.fct.default]).
// The call that supplies the argument does not need it.
struct NoMembers {};

template<class T>
struct Holder {
  int get(int x = T::missing) { return x; } //! error
};

int supplied() { return Holder<NoMembers>{}.get(1); }
int defaulted() { return Holder<NoMembers>{}.get(); }
