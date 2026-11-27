// #356: a default argument of a function template is instantiated only when a call uses it
// ([temp.inst]: default arguments are not instantiated unless needed; [dcl.fct.default]).
// Every call here supplies the argument, so the ill-formed dependent defaults (T() for a T
// without a default constructor, a missing member) are never instantiated.
struct NoDefault { NoDefault(int) {} };

template<class T>
int take(T t = T()) { (void)t; return 1; }

template<class T>
int count(int n = T::missing) { return n; }

struct Holder {
  template<class T> int f(T t = T()) { (void)t; return 2; }
  template<class T> static int s(int n = T::missing) { return n; }
};

template<class T>
struct Box {
  template<class U> int f(int n = T::missing + U::missing) { return n; }
};

int use() {
  Holder h;
  return take(NoDefault(1)) + take<NoDefault>(2) + count<NoDefault>(3) + h.f(NoDefault(4)) +
         Holder::s<NoDefault>(5) + Box<NoDefault>{}.f<int>(6);
}
