namespace assoc { struct C { int member; }; }
template<int N> int f(assoc::C c) { return c + (true ? N : 0); } //! error
namespace assoc { int operator+(const C&, int); }
int use() { return f<1>(assoc::C{1}); }
