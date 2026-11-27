// #356: a default argument of a member function or constructor of a class template is
// instantiated only when a call uses it ([temp.inst]: default arguments are not instantiated
// unless needed; [dcl.fct.default]). Every call here supplies the argument, so the ill-formed
// dependent defaults are never instantiated.
struct NoMembers {};

template<class T>
struct Holder {
  Holder(int x = T::missing) : v(x) {}
  Holder(const char*, int y = T::also_missing()) : v(y) {}
  int get(int x = T::missing) { return x; }
  static int twice(int x = T::nested::value) { return 2 * x; }
  template<class U> int with(U u, int x = T::missing + U::missing) { return x; }
  int v;
};

int use() {
  Holder<NoMembers> h(1);
  Holder<NoMembers> h2("two", 2);
  return h.get(3) + h2.v + Holder<NoMembers>::twice(4) + h.with(5, 6);
}
