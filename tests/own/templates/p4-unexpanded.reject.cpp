// ζ p4 (#88): a function parameter pack named outside a pack expansion, and an expansion whose
// pattern names no pack ([temp.variadic]/5).
template<class... Ts> int f(Ts... xs) { return xs; } //! error
int g(int x) { return x; }
template<class T> int h(T x) { return g(x...); } //! error
int main() { return 0; }
