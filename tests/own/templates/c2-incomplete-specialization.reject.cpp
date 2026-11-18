// ζ c2 (#88): a class template that is declared but never defined can be named in a pointer, but
// a variable of the specialization's type needs a complete type, and the specialization cannot
// be instantiated without a definition ([temp.inst]/2, [basic.def], [basic.types.general]).
template<class T>
struct Later;

Later<int>* ok = nullptr;
Later<int> bad; //! error
