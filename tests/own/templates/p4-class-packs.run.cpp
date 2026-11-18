// ζ p4 (#88): class templates with a pack: an empty one, member functions whose parameter pack
// is the class's, a partial specialization matching a pack, and variable templates with a pack
// folded over a partial specialization ([temp.variadic], [temp.class], [temp.spec.partial]).
// EXIT: 0
template<class... Ts> struct S {
  int f(Ts... xs) { return (0 + ... + xs); }
  int n() const { return sizeof...(Ts); }
};
template<class T> struct Head { int first() const { return 7; } };
template<class... Ts> struct List { };
template<class T, class... Us> struct Head<List<T, Us...>> { int first() const { return 100 + sizeof...(Us); } };
template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
template<class T, class... Us> constexpr bool any_of = (same<T, Us> || ...);
static_assert(any_of<int, char, int> && !any_of<int> && !any_of<int, long>);
int main() {
  S<int, int> s;
  S<> e;
  if (s.f(1, 2) != 3 || s.n() != 2 || e.n() != 0 || e.f() != 0) return 1;
  Head<List<int, char, long>> hl;
  Head<int> hi;
  if (hl.first() != 102 || hi.first() != 7) return 2;
  return 0;
}
