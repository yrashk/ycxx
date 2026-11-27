// #1400 (review): fold expressions in mem-initializers and nested in a body's operands
// ([expr.prim.fold], [temp.variadic]): the fold takes the pack, so the enclosing operand names
// none. `s((a + ...))` was reported as using `a` without '...'.
// EXIT: 0
int id(int x) { return x; }
int g(int a, int b, int c) { return a + b + c; }
struct Right { int s; template<class... A> Right(A... a) : s((a + ...)) {} };
struct Left { int s; template<class... A> Left(A... a) : s((... + a)) {} };
struct Init { int s; template<class... A> Init(A... a) : s((10 + ... + a)) {} };
template<class... A> int body(A... a) { return id((a + ...)); }
template<class... A> int braced(A... a) { int s{(a * ...)}; return s; }
template<class... B> int nested(int x, B... b) { return id((g(x, b, b) + ...)); }
int main() {
  if (Right(3, 4).s != 7) return 1;
  if (Left(3, 4, 5).s != 12) return 2;
  if (Init(1, 2).s != 13) return 3;
  if (body(1, 2, 3) != 6) return 4;
  if (braced(2, 3) != 6) return 5;
  if (nested(1, 2, 3) != 12) return 6;
  return 0;
}
