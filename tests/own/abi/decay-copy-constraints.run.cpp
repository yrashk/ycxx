// #286: function templates whose requires-clause holds a decay-copy ([expr.type.conv]/1),
// mangled as a conversion to the `auto` placeholder (Itanium C++ ABI §5.1.6): `cv Da e` for
// `auto(e)` and `tl Da e E` for `auto{e}`, inside the constraint's `Q` (§5.1.4.5). Clang and GCC
// agree on each name here. The Clang peer only declares the templates, so its calls link
// against the specializations this object defines.
// PEER: decay-copy-constraints-peer.cpp
// EXIT: 0
template<class T> T& lvalue() noexcept;
struct Range { int* begin(); int* data(); };

template<class T> requires requires { auto(lvalue<T>().begin()); } int parens(T&) { return 1; }
template<class T> requires requires { { auto{lvalue<T>().data()} }; } int braces(T&) { return 2; }

int peer();

int main() {
  Range r;
  if (parens(r) + braces(r) != 3) return 1;
  return peer();
}
