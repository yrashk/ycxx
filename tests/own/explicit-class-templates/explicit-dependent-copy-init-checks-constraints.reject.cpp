// A constructor template with a value-dependent explicit-specifier still checks its associated
// constraints before substituting the explicit-specifier ([temp.deduct.general]/5, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));  //! error
  static constexpr bool value = true;
};

struct Choice {
  int tag;
  template<class T> requires Trap<T>::value explicit(!__is_same(T, long)) Choice(T) : tag(1) {}
  Choice(long) : tag(2) {}
};

int main() {
  Choice x = 1;
  return x.tag;
}
