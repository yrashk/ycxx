// #565: a lambda body can contain a potentially evaluated call inside a decltype operand.
template<class T> int declared() {
  static_assert(false, "body instantiated"); //! error
  return 0;
}

void check() {
  using Closure = decltype([]() -> int { return declared<int>(); });
  static_assert(sizeof(Closure) > 0);
}
