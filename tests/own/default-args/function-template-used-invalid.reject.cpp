// #356: an ill-formed dependent default argument of a function template is instantiated, and
// diagnosed, when a call uses it: T() for a T without a default constructor
// ([temp.inst], [dcl.fct.default], [dcl.init.general]). The call that supplies the argument does
// not need it.
struct NoDefault { NoDefault(int) {} };

template<class T>
int take(T t = T()) { (void)t; return 1; } //! error

int supplied() { return take(NoDefault(1)); }
int defaulted() { return take<NoDefault>(); }
