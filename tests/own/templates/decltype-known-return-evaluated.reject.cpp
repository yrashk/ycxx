// #565: the same specialization still needs its definition when called in an evaluated context.
template<class T> int declared() noexcept {
  static_assert(false, "body instantiated"); //! error
  return 0;
}

using Result = decltype(declared<int>());
Result evaluate() { return declared<int>(); }
