// ζ s7 (#88): a const static data member of the current instantiation is value-dependent in the
// template ([temp.dep.constexpr]/2.3); partial specializations and constraints read the value of
// each specialization's.
// EXIT: 0
template<class T> struct P { static constexpr int v = 0; };
template<class T> struct P<T*> { static constexpr int v = 1 + P<T>::v; };
template<class T> struct A { static constexpr int N = sizeof(T) / sizeof(char); int size() const { return N; } };
template<class T> struct B { static constexpr bool ok = __is_same(T, int); };
template<class T> requires B<T>::ok int g(T) { return 1; }
template<class T> int g(T) { return 2; }
static_assert(P<int**>::v == 2 && P<int>::v == 0);
int main() { A<short> a; return g(1) == 1 && g('c') == 2 && a.size() == 2 ? 0 : 1; }
