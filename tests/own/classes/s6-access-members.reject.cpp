// #119: a private member named outside its class and its friends ([class.access]/1, /4):
// through `.` and `->`, by a qualified name, a static member, an enumerator and a type of the
// class, the private overload a call chooses, and a private constructor.
class A {
  int x = 1;
  static int s;
  enum E { e };
  using T = int;
  int f(int) { return 1; }
  A(int) {}
public:
  A() {}
  int f(long) { return 2; }
  int y = 0;
};
int A::s = 0;
int use(A& a, A* p) {
  int r = a.x;   //! error
  r += p->x;     //! error
  r += A::s;     //! error
  r += A::e;     //! error
  A::T t = 0;    //! error
  r += a.f(1);   //! error
  r += a.f(1L);
  return r + t + a.y;
}
A make() { return A(3); }  //! error
int main() { return 0; }
