namespace assoc { struct A { int operator+(int) & { return 1; } }; }
template<class T> int f(T& value) { return value + 0; }
namespace assoc { int operator+(const A&, int) { return 2; } }
int use() { assoc::A a; return f(a); }

namespace tied {
struct B { constexpr int operator+(int) & { return 1; } };
}
template<class T> constexpr int tied_receiver(T& value) { return value + 0; }
namespace tied { constexpr int operator+(B&, long) { return 2; } }
int use_tied_receiver() { tied::B b; return tied_receiver(b); }
