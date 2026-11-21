// #565: decltype needs the declared type of the call, so the template body stays uninstantiated.
template<class T> int declared() noexcept {
  static_assert(false, "body instantiated");
  return 0;
}

using Result = decltype(declared<int>());
static_assert(__is_same(Result, int));
