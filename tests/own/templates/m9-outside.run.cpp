// ζ m9 (#88, #314): member templates defined outside their class ([temp.mem]/1): `template<class T>
// R S::f(T)` for a class, `template<class U> template<class T> R W<U>::f(T)` for a class template,
// whose body names both levels' parameters; a constructor template likewise.
// EXIT: 0
struct S { template<class T> T get(T t) const; template<class T> S(T); int v = 0; };
template<class T> T S::get(T t) const { return t + v; }
template<class T> S::S(T t) : v(int(t)) {}
template<class U> struct W { U u; template<class T> int f(T); };
template<class U> template<class T> int W<U>::f(T t) { return int(sizeof(U)) + int(t); }
int main() { S s(2); W<int> w; return s.get(3) + w.f(1) - 10; }
