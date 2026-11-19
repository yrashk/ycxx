// ζ p4 (#88): deducing packs: a trailing function parameter pack from the remaining arguments,
// forwarding references in it, an explicitly specified prefix extended by deduction, a pack in
// a class template specialization's arguments, and partial ordering, where a template without
// a trailing pack is more specialized ([temp.deduct.call]/1, [temp.arg.explicit]/9,
// [temp.deduct.type]/9, [temp.deduct.partial]).
// EXIT: 0
template<class... Ts> struct List { int size() const { return sizeof...(Ts); } };
template<class... Ts> int length(const List<Ts...>&) { return sizeof...(Ts); }
template<class T, class... Ts> int rest(const List<T, Ts...>&) { return sizeof...(Ts); }
template<class... Ts> int h(Ts... xs) { return sizeof...(Ts) * 10 + (0 + ... + xs); }
template<class... Ts> int forwarded(Ts&&... xs) { return sizeof...(xs); }
template<class T> int p(T) { return 1; }
template<class... Ts> int p(Ts...) { return 2; }
template<class T> int q(T) { return 1; }
template<class T, class... Us> int q(T, Us...) { return 2; }
int main() {
  List<int, char, long> l;
  if (length(l) != 3 || rest(l) != 2 || length(List<>{}) != 0) return 1;
  if (h<char>(1, 2, 3) != 36) return 2;
  int x = 1;
  if (forwarded(x, 2, x) != 3) return 3;
  if (p(1) != 1 || p(1, 2) != 2 || p() != 2) return 4;
  if (q(1) != 1 || q(1, 2) != 2) return 5;
  return 0;
}
