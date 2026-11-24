// #1099: a builtin argument's pack expansion must name a pack, and a pack must be expanded
// ([temp.variadic]/5, /6).
int x;
template <class... A> decltype(sizeof 0) no_pack() { return __builtin_strlen(x...); } //! error
template <class... A> decltype(sizeof 0) unexpanded(A... a) { return __builtin_strlen(a); } //! error
