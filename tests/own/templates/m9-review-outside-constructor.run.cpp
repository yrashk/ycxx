// ζ m9 (#88, #314), from the step's review: a class template's constructor template defined outside it ([temp.mem]/1).
// EXIT: 0
template<class U> struct W { U u; template<class T> W(T, T); };
template<class U> template<class T> W<U>::W(T a, T b) : u(U(b - a)) {}
struct S { int v; template<class T> S(T); };
template<class T> S::S(T t) : v(int(t)) {}
int main() { W<int> w(1L, 4L); S s(2); return w.u + s.v - 5; }
