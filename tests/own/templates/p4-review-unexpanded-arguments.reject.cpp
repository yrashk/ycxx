// ζ p4 (#88), from the step's review: a pack named in a call's argument or an initializer list
// without `...` ([temp.variadic]/5).
int g(int x) { return x; }
template<class... Ts> int f(Ts... xs) { return g(xs); } //! error
template<int... Is> int h() { return g(Is); } //! error
template<int... Is> int k() { int a[] = {Is}; return a[0]; } //! error
int main() { return 0; }
