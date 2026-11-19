// #220, from the review: an unscoped enumeration without a fixed type promotes to the first of
// int, unsigned int, long, unsigned long that holds all its values ([conv.prom]/3), whatever
// its underlying type; arithmetic and overload resolution see that type ([over.ics.rank]).
// EXIT: 0
enum A { a0 };
enum B { b0 = 0xFFFFFFFF };
enum C { c0 = 0x7FFFFFFF };
enum D { d0 = -1 };
enum E { e0 = 0x80000000 };
static_assert(a0 - 1 < 0);
static_assert(__is_same(decltype(+a0), int));
static_assert(__is_same(decltype(+b0), unsigned int));
static_assert(__is_same(decltype(+c0), int));
static_assert(__is_same(decltype(+e0), unsigned int));
static_assert(__is_same(decltype(a0 + 0), int));
static_assert(__is_same(decltype(a0 + 0u), unsigned int));
int f(int) { return 1; }
int f(unsigned) { return 2; }
int g(long) { return 1; }
int g(unsigned) { return 2; }
struct S { enum N { n0, n1 = 5 } m; char c; };

static_assert(__is_same(__underlying_type(S::N), unsigned int));
static_assert(static_cast<int>(a0) == 0);
int main() {
  if (f(a0) != 1) return 1;
  if (f(c0) != 1) return 2;
  if (f(b0) != 2) return 3;
  if (f(e0) != 2) return 4;
  A a = a0;
  if (a - 1 >= 0) return 5;
  if (-a0 > 0) return 6;
  return 0;
}
