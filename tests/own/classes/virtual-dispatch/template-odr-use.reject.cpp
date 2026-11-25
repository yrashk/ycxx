// #1153 [basic.def.odr], [temp.inst]: a virtual call requires the named function's definition,
// whether or not the unit installs a table.
template <class T> struct S {
  virtual T f();
};
template <class T> T S<T>::f() { return T::bad(); } //! error
int g(S<int>* p) { return p->f(); }
