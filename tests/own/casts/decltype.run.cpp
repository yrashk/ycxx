// #133, from the step's review: decltype names the declared type of a qualified, using-declared
// or static data member name and of a function, and of a member access whose object is in
// parentheses ([dcl.type.decltype]/1); sizeof of an expression that begins like a type-id.
// EXIT: 0
namespace N { int v; }
struct S { static int sm; int m; };
int S::sm = 0;
namespace M { int w; }
int f(int);
int f(int v) { return v; }
struct Q { static int qs; int k() { decltype(qs) a = 5; return a; } };
int Q::qs = 0;
int main() {
  decltype(N::v) a = 5;
  decltype(S::sm) b = 5;
  using M::w;
  decltype(w) c = 5;
  int w2 = 0;
  decltype(M::w) d = 5;
  decltype(f)* p = &f;
  S so{}; S* sp = &so;
  decltype((so).m) e = 5;
  decltype((sp)->m) g = 5;
  decltype((*sp).m) h = 5;
  unsigned long z = sizeof(int(1)) + sizeof(char{}) - 5;
  Q q;
  return a + b + c + d + e + g + h - 35 + (p == &f ? 0 : 1) + int(z) + q.k() - 5 + w2;
}
