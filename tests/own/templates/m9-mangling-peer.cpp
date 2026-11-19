// The peer of m9-mangling.run.cpp, compiled by Clang: the member templates' definitions,
// explicitly instantiated.
struct S {
  int v;
  template<class T> T get(T t) const;
  template<class T> static int st(T);
  template<class T> S(T t);
};
template<class T> T S::get(T t) const { return t; }
template<class T> int S::st(T) { return 2; }
template<class T> S::S(T t) : v(int(t)) {}
template<class U> struct W {
  template<class T> int f(T);
  template<class T, class R> R g(T*, R);
};
template<class U> template<class T> int W<U>::f(T) { return int(sizeof(U)); }
template<class U> template<class T, class R> R W<U>::g(T*, R r) { return r; }
template int S::get<int>(int) const;
template long S::get<long>(long) const;
template int S::st<char>(char);
template S::S(int);
template int W<int>::f<char>(char);
template long W<char>::g<int, long>(int*, long);
