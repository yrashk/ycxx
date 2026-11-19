// ζ p4 (#88): non-type parameter packs: in a variable template's fold, in a class template whose
// pack's type is an earlier parameter, deduced from a specialization's arguments, and
// `sizeof...` as a template argument ([temp.param], [temp.deduct.type]/9, [expr.sizeof]).
// EXIT: 0
template<int... Ns> constexpr int total = (Ns + ... + 0);
static_assert(total<1, 2, 3> == 6 && total<> == 0);
template<int... Ns> struct I { int size() const { return sizeof...(Ns); } };
template<int... Ns> int sum(const I<Ns...>&) { return (0 + ... + Ns); }
template<class T, T... Is> struct Seq { int size() const { return sizeof...(Is); } };
template<int N> constexpr int value = N;
template<class... Ts> int how_many() { return value<sizeof...(Ts)>; }
int main() {
  I<4, 5> i;
  if (i.size() != 2 || sum(i) != 9) return 1;
  Seq<long, 1, 2, 3> s;
  if (s.size() != 3) return 2;
  return how_many<int, char, long>() == 3 ? 0 : 3;
}
