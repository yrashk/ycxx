namespace A { struct C { int n; }; }
template<int N> int f(A::C c) { return c + (N + 1); } //! error
namespace A { int operator+(const C&, int); }
int use() { return f<1>(A::C{1}); }
