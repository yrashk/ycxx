// Member templates' defaults are instantiated only for a call that uses them ([temp.inst]/12):
// defaults of a class template's member templates that use each other are not diagnosed while
// no call uses them ([dcl.fct.default], [temp.mem]).
template<class T> struct A {
  template<class U> static int f(U, int x = g(1)) { return x; }
  template<class U> static int g(U, int x = f(1)) { return x; }
};
int y = A<int>::f(1, 2) + A<int>::g(1, 3);
