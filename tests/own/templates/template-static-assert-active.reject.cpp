// #241: after instantiation the false assertion in an active branch fails.
template<class T> void checked() {
  if constexpr (sizeof(T) == sizeof(char)) {
    static_assert(false, "active assertion"); //! error
  }
}

void instantiate() { checked<char>(); }
