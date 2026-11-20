// #241: calling the generic lambda instantiates its assertion.
template<class T> void outer() {
  auto checked = []<class U>(U) {
    static_assert(false); //! error
  };
  checked(0);
}

void instantiate() { outer<int>(); }
