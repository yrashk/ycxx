// #153: as with an inline namespace, a namespace inside an unnamed one keeps its own inline
// namespace set ([namespace.unnamed], [namespace.def.general]/7).
// EXIT: 0
namespace a { namespace { namespace b { inline namespace c { int x = 9; } } } }
namespace a { int f() { return b::x; } }
int main() {
  if (a::b::x != 9) return 1;
  if (a::b::c::x != 9) return 2;
  if (a::f() != 9) return 3;
  return 0;
}
