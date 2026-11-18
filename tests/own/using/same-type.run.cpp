// #133, from the step's review: a using-declaration and a typedef-name or class of the scope that
// name the same type do not conflict ([dcl.typedef]/5, [namespace.udecl]/10), as <cstddef>'s
// `using ::size_t;` beside a typedef of it; nor do the declarations of an inline or unnamed
// member namespace, which do not inhabit the enclosing scope ([basic.scope.scope]/4).
// EXIT: 0
namespace N { typedef int T; }
using N::T;
typedef int T;
struct SameS { int a; };
namespace K { typedef SameS SameS; }
using K::SameS;
namespace V { int v = 1; }
namespace M { inline namespace I { using V::v; } int v = 2; }
namespace { using V::v; }
int v = 2;
int main() {
  T t = 1;
  SameS s{2};
  return t + s.a + M::I::v + V::v - 5;  // `M::v` and `::v` would be ambiguous
}
