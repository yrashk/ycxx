// [except.spec]: the implicit exception specification still applies to a destructor.
template<class T> struct Cell { ~Cell() noexcept(false) {} };
template<> Cell<int>::~Cell() {} //! error
