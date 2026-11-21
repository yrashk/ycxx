namespace A { struct C { int n; }; }
template<class B> int f(B b, A::C c) { return c + (b ? 1 : 2); }
namespace A { int operator+(const C&, int) { return 3; } }
int use() { return f(true, A::C{1}); }
