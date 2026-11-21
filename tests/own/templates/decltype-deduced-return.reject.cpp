// #565: deducing a placeholder return type requires instantiating the function body.
template<class T> auto deduced() {
  static_assert(false, "body instantiated"); //! error
  return T{};
}

using Result = decltype(deduced<int>());
