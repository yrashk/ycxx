// #119: where private members may be named ([class.access]/1–2, [class.friend]): in member
// functions, inline or defined outside the class, static member functions, default member
// initializers and static data members' initializers, a friend function declared before its
// definition, a friend class (named before it is declared), and another class's member function
// made a friend.
// EXIT: 0
class A;
struct G { int peek(A& a); };
class A {
  int x = 4;
  static const int k = 3;
  int y = k + 1;
  int twice() const { return x * 2; }
  A(int v) : x(v) {}
  friend int get(const A&);
  friend class B;
  friend int G::peek(A&);
public:
  A() {}
  int sum() const;
  static A make(int v) { return A(v); }
  static const int t;
};
const int A::t = k + 2;
int A::sum() const { return twice() + y; }
int get(const A& a) { return a.x; }
class B {
public:
  int g(A& a) { return a.x + A::k; }
};
int G::peek(A& a) { return a.y; }
int main() {
  A a = A::make(5);
  if (a.sum() != 14) return 1;
  if (get(a) != 5) return 2;
  if (B().g(a) != 8) return 3;
  G g;
  if (g.peek(a) != 4) return 4;
  if (A::t != 5) return 5;
  return 0;
}
