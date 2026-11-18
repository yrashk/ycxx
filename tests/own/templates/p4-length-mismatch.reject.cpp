// ζ p4 (#88): the packs one expansion names have the same length ([temp.variadic]/7).
template<class... Ts> struct List { };
template<class T, class U> int pair() { return 1; }
template<class... A, class... B> int pairs(List<A...>, List<B...>) { return (0 + ... + pair<A, B>()); } //! error
int main() { return pairs(List<int>{}, List<int, char>{}); }
