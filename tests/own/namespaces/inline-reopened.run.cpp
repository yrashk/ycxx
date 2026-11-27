// #153: inline-ness belongs to the namespace, not to a definition ([namespace.def.general]/7): an
// extension of an inline namespace written without `inline` still adds members of the enclosing one.
// EXIT: 0
namespace a { inline namespace b {} }
namespace a { namespace b { int h() { return 5; } } }
namespace a::b { int k() { return 6; } }
namespace q { inline namespace v { namespace r { inline namespace c {} } } }
namespace q::v::r::c { int m() { return 7; } }
int main() {
  if (a::h() != 5 || a::b::h() != 5) return 1;
  if (a::k() != 6) return 2;
  if (q::r::m() != 7 || q::v::r::m() != 7) return 3;
  return 0;
}
