// Keep the invalid body and the demand together for compilers that report either location.
template<class T> struct Only {
  static constexpr int unused() { return T::missing; } }; static_assert(Only<int>::unused() == 0); //! error
