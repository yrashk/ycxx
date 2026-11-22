// #565: substitute a dependent qualified alias and preserve the reference return type.
namespace signatures {
template<class T> using Reference = T&&;

template<class T> signatures::Reference<T> declared(T& value) {
  static_assert(false, "body instantiated");
  return static_cast<T&&>(value);
}
}

extern int value;
using Result = decltype(signatures::declared(value));
static_assert(__is_same(Result, int&&));
