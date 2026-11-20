// #241: instantiating the enclosing function does not instantiate the
// generic lambda's call operator.
template<class T> void outer() {
  auto unused = []<class U>() { static_assert(false); };
}

void instantiate() { outer<int>(); }
