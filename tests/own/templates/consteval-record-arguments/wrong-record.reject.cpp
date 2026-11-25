// #317, [temp.arg.nontype]: the argument must have the destination record type.
struct Value { long num, den; bool ok; };
struct Other { long num, den; bool ok; };

template<Value> struct Box {};

namespace detail {
  template<class, class> consteval Other make() { return {-3, 5, true}; }
}

template<class T, class U> using Mapped = Box<detail::make<T, U>()>; //! error
using Demanded = Mapped<int, char>;
