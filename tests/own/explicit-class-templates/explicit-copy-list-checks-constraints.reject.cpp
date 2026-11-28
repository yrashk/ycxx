// Copy-list-initialization still considers explicit constructor templates and checks their
// constraints ([over.match.list]/1, [temp.deduct.general]/5, #1550).
template<class T> struct Trap {
  static_assert(!__is_same(T, int));  //! error
  static constexpr bool value = false;
};

struct Choice {
  int tag;
  template<class T> requires Trap<T>::value explicit Choice(T) : tag(1) {}
  Choice(long) : tag(2) {}
};

int main() {
  Choice x = {1};
  return x.tag;
}
