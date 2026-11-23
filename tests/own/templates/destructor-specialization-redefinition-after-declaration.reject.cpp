// [temp.expl.spec], [basic.def.odr]: redeclaring the specialization between
// its definitions does not make a second definition in the TU valid.

template<class T>
struct Cell {
  ~Cell() {}
};

template<> inline Cell<int>::~Cell() {}
template<> inline Cell<int>::~Cell();
template<> inline Cell<int>::~Cell() {} //! error
