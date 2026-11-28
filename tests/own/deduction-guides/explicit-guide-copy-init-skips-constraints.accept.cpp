// Copy-initialization excludes known explicit implicit and user deduction guides before
// deduction and constraint satisfaction ([over.match.class.deduct]/1, [over.match.copy]/1.1, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));
  static constexpr bool value = true;
};

template<class T> concept NotTrap = Trap<T>::value;

template<class U> struct Box {
  int tag;
  template<class X> requires Trap<X>::value explicit constexpr Box(X) : tag(1) {}
  constexpr Box(long) : tag(2) {}
};
Box(long) -> Box<long>;
template<class T> requires NotTrap<T> explicit Box(T) -> Box<T*>;

template<class U> requires Trap<U>::value struct ConstrainedBox {
  int tag;
  explicit constexpr ConstrainedBox(U) : tag(1) {}
  constexpr ConstrainedBox(long, int = 0) : tag(2) {}
};
ConstrainedBox(long) -> ConstrainedBox<long>;

template<class U = long> struct DefaultedBox {
  int tag;
  template<class X> requires Trap<X>::value explicit constexpr DefaultedBox(X) : tag(1) {}
  constexpr DefaultedBox(long, int = 0) : tag(2) {}
};

constexpr bool check_guides() {
  Box a = 1;
  if (a.tag != 2) return false;
  ConstrainedBox b = 1;
  if (b.tag != 2) return false;
  DefaultedBox c = 1;
  if (c.tag != 2) return false;
  return true;
}

static_assert(check_guides());
