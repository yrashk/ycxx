// #153: members of nested inline namespaces are members of every namespace above them up to the
// first one that is not inline ([namespace.def.general]/7): `x` is found in `a::b::c` as well as in
// `a::b`.
// EXIT: 0
namespace a::b::inline c::inline d { int x = 7; }
namespace a { namespace b { inline namespace c { inline namespace d { int y() { return x + 1; } } } } }
int main() {
  if (a::b::x != 7) return 1;
  if (a::b::c::x != 7) return 2;
  if (a::b::c::d::x != 7) return 3;
  if (a::b::y() != 8 || a::b::c::y() != 8) return 4;
  return 0;
}
