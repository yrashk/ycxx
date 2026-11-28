// A constructor template with explicit(false) is converting and still has its constraints
// checked in copy-initialization ([over.match.copy]/1.1, [temp.deduct.general]/5, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));  //! error
  static constexpr bool value = true;
};

struct Choice {
  int tag;
  template<class T> requires Trap<T>::value explicit(false) Choice(T) : tag(1) {}
  Choice(long) : tag(2) {}
};

int main() {
  Choice x = 1;
  return x.tag;
}
