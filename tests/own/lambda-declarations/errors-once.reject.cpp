// A generic lambda's error in a variable template is said where its definition is instantiated, once.
template <class T>
constexpr auto f = [](auto x) { return T::bad; };  //! error
void g() { f<int>(1); }
