namespace A { struct C { int n; }; }
template<int N> int f(A::C c) { return c + (true ? N : 0L); } //! error
namespace A { int operator+(const C&, long); }
int use() { return f<1>(A::C{1}); }
