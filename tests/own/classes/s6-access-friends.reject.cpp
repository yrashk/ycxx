// #119: friendship is granted, not inherited by other functions or classes ([class.friend]/1,
// /10): a friend function of A gives nothing to another function, and a friend class of A gives
// nothing to a class that is not one.
class A {
  int x = 1;
  friend int ok(A&);
  friend class B;
};
int ok(A& a) { return a.x; }
int other(A& a) { return a.x; }  //! error
class B { public: int g(A& a) { return a.x; } };
class C { public: int g(A& a) { return a.x; } };  //! error
int main() { return 0; }
