// #1153 [class.virtual], [except.spec]: a class template specialization's destructor overrides
// its base's. It cannot be less restrictive than it, declared or implicit.
struct Base {
  virtual ~Base() {}
};
struct Throwing {
  ~Throwing() noexcept(false) {}
};

template <class T> struct Declared : T {
  ~Declared() noexcept(false) {} //! error
};
Declared<Base> declared;
