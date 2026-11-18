// #133: typedef-names and alias-declarations at namespace and block scope, redeclared as the
// same type, through pointers, arrays and function types, qualified, and hidden by an inner
// block's own ([dcl.typedef], [basic.scope.hiding]).
// EXIT: 0
typedef int I;
typedef I* IP, IA[3];
using U = unsigned long;
typedef int I;               // the same type again: fine
struct S { int x; };
typedef struct S S;          // [dcl.typedef]/5
typedef S T;
namespace N { typedef long L; using P = const char*; }
typedef int F(int);
F g;
int g(int v) { return v; }
enum E { a, b };
typedef E EE;
int h(I x, U y) { I yy = y; return x + yy; }
int main() {
  I i = 3;
  I j = 3;
  IP p = &j;
  IA arr = {1, 2, 3};
  T t{4};
  N::L l = 5;
  N::P s = "ok";
  int L2 = l;
  typedef short Sh;
  using D = int;
  Sh sh = 6;
  D d = 1;
  EE e = b;
  {
    typedef long Sh;
    Sh big = 1;
    I b2 = big; i += b2;
  }
  return h(*p, 1) + arr[2] + t.x + L2 + sh + d + e + i + (s[0] == 'o') + g(1) - 30;
}
