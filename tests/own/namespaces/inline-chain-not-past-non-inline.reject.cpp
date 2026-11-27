// #153: lookup through nested inline namespaces stops at `b`, which is not inline
// ([namespace.def.general]/7).
namespace a::b::inline c::inline d { int x = 7; }
int f() { return a::x; } //! error
int main() { return 0; }
