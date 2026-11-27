// #356: an ill-formed dependent default argument of a member function template is instantiated,
// and diagnosed, when a call uses it ([temp.inst], [temp.mem], [dcl.fct.default]). The call that
// supplies the argument does not need it.
struct NoMembers {};

struct Holder {
  template<class T> int f(int n = T::missing) { return n; } //! error
};

int supplied() { return Holder{}.f<NoMembers>(1); }
int defaulted() { return Holder{}.f<NoMembers>(); }
