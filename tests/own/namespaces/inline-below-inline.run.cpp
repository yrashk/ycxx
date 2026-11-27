// #153, from the <iostream> chain (#776): a namespace `r` inside an inline namespace keeps its own
// inline namespace set, so qualified lookup into `r` finds `c`'s members however `r` is named
// ([namespace.def.general]/7, [namespace.qual]). libycxx's `std::__y1::ranges::__cpo` is this shape.
// EXIT: 0
namespace s { inline namespace y { namespace r { inline namespace c { int im = 3; } } } }
int f() { return s::r::im; }
int g() { return s::y::r::im; }
int h() { return s::y::r::c::im; }
namespace s { inline namespace y { int u() { return r::im + 1; } } }  // unqualified, from inside s::y
namespace s { inline namespace y { namespace r { int v() { return im + 2; } } } }
namespace s { int w() { return r::im + 3; } }
int main() {
  if (f() != 3) return 1;
  if (g() != 3) return 2;
  if (h() != 3) return 3;
  if (s::u() != 4 || s::y::u() != 4) return 4;
  if (s::r::v() != 5) return 5;
  if (s::w() != 6) return 6;
  return 0;
}
