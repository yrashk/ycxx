namespace assoc { struct A { int operator+(int) & { return 1; } }; }
template<class T> int f(T& value) { return value + 0; }
namespace assoc { int operator+(const A&, int) { return 2; } }
int use() { assoc::A a; return f(a); }
