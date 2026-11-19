// ζ a5 (#88): alias templates at namespace scope and in a namespace: their uses are the types
// they stand for with the arguments substituted, as variables' and parameters' types; a pack
// argument; deduction through an alias of a deducible type and none through `typename X<T>::type`
// ([temp.alias], [temp.deduct.type]/5).
// EXIT: 0
template<class T> struct Rm { using type = T; };
template<class T> struct Rm<T&> { using type = T; };
template<class T> using Rm_t = typename Rm<T>::type;
template<class T> using Ptr = T*;
template<class... Ts> struct List { int size() const { return sizeof...(Ts); } };
template<class... Ts> using L = List<Ts..., char>;
template<class T> T get(Rm_t<T> x) { return x; }
template<class T> int deref(Ptr<T> p) { return *p; }
namespace n { template<class T> using Id = T; }
int main() {
  Rm_t<int&> a = 1;
  Ptr<int> p = &a;
  L<int, long> l;
  L<> l0;
  n::Id<int> b = 2;
  if (deref(p) != 1 || l.size() != 3 || l0.size() != 1 || get<int>(5) != 5) return 1;
  return a + b == 3 ? 0 : 2;
}
