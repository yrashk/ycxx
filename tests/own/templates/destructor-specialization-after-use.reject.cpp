// [temp.expl.spec], [temp.inst], [class.dtor]: destruction of an automatic
// object uses the primary destructor before a later explicit specialization.
// XFAIL: clang Clang accepts this specialization after use ([temp.expl.spec], no diagnostic required).

template<class T>
struct Cell {
  ~Cell() {}
};

void before_specialization() {
  Cell<int> object;
}

template<> Cell<int>::~Cell() {} //! error
