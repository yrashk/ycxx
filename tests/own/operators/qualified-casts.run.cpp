// #133, from the step's review: `N::T(args)` and `N::T{…}` are functional casts when N::T is a
// type ([expr.type.conv]); a function of the same name hides the class ([basic.scope.hiding]/2),
// so `::stat(5, &b)` calls it, as the C library's `stat` needs.
// EXIT: 0
struct stat { int x; };
int stat(int k, struct stat* s) { s->x = k; return k + 1; }
namespace N {
struct sigaction { int y; };
int sigaction(int k) { return k * 2; }
enum class E : unsigned char { a = 1 };
struct P { int v; };
}
int main() {
  struct stat b;
  int r = ::stat(5, &b);
  int q = N::sigaction(4);
  N::E e = N::E(1);
  N::P p = N::P{7};
  return r - 6 + b.x - 5 + q - 8 + (e == N::E::a ? 0 : 1) + p.v - 7;
}
