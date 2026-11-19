// #119, from the review: a member's definition outside its class, and a friend's, may name the
// class's private types in their decl-specifiers ([class.access]/6–7); a friend may be a function
// named with `::` or a namespace qualifier, declared earlier ([namespace.memdef]/3).
// EXIT: 0
struct S;
int peek(S& s);
namespace N { int g(S&); }
struct S {
private:
  using T = int;
  static T z;
  T v = 2;
  T x = 3;
  friend T look(S&);
  friend int ::peek(S&);
  friend int N::g(S&);
public:
  T f(T);
};
S::T S::z = 1;
S::T S::f(T a) { return a + v + z; }
S::T look(S& s) { return s.x; }
int peek(S& s) { return s.x * 10; }
int N::g(S& s) { return s.v * 100; }
int main() {
  S s;
  if (s.f(4) != 7) return 1;
  if (look(s) != 3) return 2;
  if (peek(s) != 30) return 3;
  if (N::g(s) != 200) return 4;
  return 0;
}
