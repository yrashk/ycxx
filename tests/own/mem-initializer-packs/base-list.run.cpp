// #1400: a mem-initializer-list pack expansion over a base-class pack ([class.base.init],
// [temp.variadic]): `Bs(x)...`, `Bs{x}...`, a pattern naming the base pack in its argument, an
// expansion inside each base's argument list, a base pack expanded together with a function
// parameter pack, and an ordinary member initializer beside the expansion. Bases are initialized
// in declaration order, then members, whatever the order of the mem-initializers
// ([class.base.init]).
// XFAIL: ycxx needs a pack expansion in a class template's base-specifier-list (#308)
// EXIT: 0
int trace;
int mark(int id) { trace = trace * 10 + id; return id; }

template<int Id> struct B {
  static constexpr int id = Id;
  int got;
  B(int x) : got(x) { mark(Id); }
  B(int x, int y) : got(x * 100 + y) { mark(Id); }
};
struct First { int got; First(int x) : got(x) { mark(1); } };

template<class... Bs> struct Paren : Bs... { Paren(int x) : Bs(x)... {} };
template<class... Bs> struct Braced : Bs... { Braced(int x) : Bs{x}... {} };
template<class... Bs> struct Named : Bs... { Named(int x) : Bs(x + Bs::id)... {} };
template<class... Bs> struct Inner : Bs... {
  template<class... A> Inner(A... a) : Bs(a...)... {}
};
template<class... Bs> struct Paired : Bs... {
  template<class... A> Paired(A... a) : Bs(a)... {}
};
template<class... Bs> struct Mixed : First, Bs... {
  int member;
  Mixed(int x) : member(mark(9)), Bs(x)..., First(x) {}
};

int main() {
  trace = 0;
  Paren<B<2>, B<3>, B<4>> paren(7);
  if (trace != 234) return 1;
  if (paren.B<2>::got != 7 || paren.B<3>::got != 7 || paren.B<4>::got != 7) return 2;

  trace = 0;
  Braced<B<4>, B<2>> braced(8);
  if (trace != 42 || braced.B<4>::got != 8 || braced.B<2>::got != 8) return 3;

  Named<B<2>, B<5>> named(10);
  if (named.B<2>::got != 12 || named.B<5>::got != 15) return 4;

  Inner<B<2>, B<3>> inner(1, 2);
  if (inner.B<2>::got != 102 || inner.B<3>::got != 102) return 5;

  trace = 0;
  Paired<B<3>, B<2>> paired(30, 20);
  if (trace != 32 || paired.B<3>::got != 30 || paired.B<2>::got != 20) return 6;

  trace = 0;
  Mixed<B<3>, B<2>> mixed(6);
  if (trace != 1329) return 7;
  if (mixed.First::got != 6 || mixed.B<3>::got != 6 || mixed.B<2>::got != 6 || mixed.member != 9) return 8;
  return 0;
}
