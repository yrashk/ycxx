// #241: a false assertion in a class template's member function fails when
// the member function is instantiated.
template<class T> struct Checked {
  void member() { static_assert(false); } //! error
};

void instantiate() { Checked<int>{}.member(); }
