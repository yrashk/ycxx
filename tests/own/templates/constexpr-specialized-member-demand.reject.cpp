template<class T> struct Only {
  static constexpr int unused() { return T::missing; } //! error: is not a namespace or class
};
static_assert(Only<int>::unused() == 0); //! error: not a constant expression
