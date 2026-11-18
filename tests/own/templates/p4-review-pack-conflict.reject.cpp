// ζ p4 (#88), from the step's review: a deduced pack must agree with the one another parameter
// deduces ([temp.deduct.type]).
template<class... Ts> struct List {};
template<class... Ts> int f(const List<Ts...>&, Ts... xs) { return sizeof...(Ts); }
int main() { List<int, char> l; return f(l, 1L, 2L); } //! error
