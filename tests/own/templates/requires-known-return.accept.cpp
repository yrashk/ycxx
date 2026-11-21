// #565: a simple requirement checks the call without requiring its definition.
template<class T> int declared(T value) {
  static_assert(false, "body instantiated");
  return value;
}

static_assert(requires { declared(0); });
