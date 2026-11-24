// #317, [temp.param]/13, [temp.type]/1,2, [dcl.type.decltype]/1:
// different templates share one const parameter object for the same class type and value.
// EXIT: 0
struct Options {
  int count;
  bool enabled;
};

template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

template<Options V> struct First {
  static constexpr const Options* address() { return &V; }
  static_assert(same<decltype(V), Options>);
  static_assert(same<decltype((V)), const Options&>);
};

template<Options V> struct Second {
  static constexpr const Options* address() { return &V; }
};

constexpr Options named{7, true};
static_assert(First<Options{7, true}>::address() == Second<named>::address());
static_assert(First<named>::address() != Second<Options{7, false}>::address());
static_assert(First<named>::address() != Second<Options{8, true}>::address());

int main() {}
