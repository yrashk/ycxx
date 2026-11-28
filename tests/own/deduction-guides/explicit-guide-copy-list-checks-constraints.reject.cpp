// Copy-list-initialization with CTAD still considers explicit deduction guides and checks
// their associated constraints ([over.match.list]/1, [over.match.class.deduct]/1, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));  //! error
  static constexpr bool value = false;
};

template<class U = long> struct DefaultedBox {
  int tag;
  template<class X> requires Trap<X>::value explicit constexpr DefaultedBox(X) : tag(1) {}
  constexpr DefaultedBox(long, int = 0) : tag(2) {}
};

int main() {
  DefaultedBox c = {1};
  return c.tag;
}
