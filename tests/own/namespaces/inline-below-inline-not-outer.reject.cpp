// #153: `r` is not inline, so `c`'s members are members of `r` but not of `s` or `s::y`, even though
// `y` and `c` are inline ([namespace.def.general]/7).
namespace s { inline namespace y { namespace r { inline namespace c { int im = 0; } } } }
int f() { return s::im; } //! error
int g() { return s::y::im; } //! error
int main() { return 0; }
