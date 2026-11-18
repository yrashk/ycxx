// #133: using-declarations at namespace scope ([namespace.udecl]), as libycxx's <cstdio> has
// them: `using ::f;` in an inline namespace of an attributed namespace, functions with their
// overloads, typedef-names, classes and enumerations, several declarators, and a
// using-declaration of a using-declaration. Qualified and unqualified lookup find the targets.
// (The class name is unique among tests/own: a daemon session shares same-named classes, #185.)
// EXIT: 0
typedef struct UsedPt { int x; } P;
int twice(int v) { return 2 * v; }
long twice(long v) { return 3 * v; }
namespace lib { int seven() { return 7; } enum Mode { off, on }; }
namespace [[gnu::visibility("default")]] st { inline namespace v1 {
using ::P, ::twice;
using lib::seven;
using lib::Mode;
} }
namespace user { using st::seven; int eight() { return seven() + 1; } }
using st::twice;
int main() {
  st::P p{1};
  st::Mode m = lib::on;
  long l = 1;
  return st::twice(2) + twice(l) + user::eight() + p.x + m - 17;
}
