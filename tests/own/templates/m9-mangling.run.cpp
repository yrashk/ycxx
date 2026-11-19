// ζ m9 (#88, #314): member template specializations' names are Clang's (Itanium §5.1.2, §5.1.5.3):
// `N [K] <class> <name> I <args> E E` with the member's own parameters as `T_`, a constructor
// template's `C1` without a return type. The peer, compiled by Clang, defines them.
// PEER: m9-mangling-peer.cpp
// EXIT: 0
struct S {
  int v;
  template<class T> T get(T t) const;
  template<class T> static int st(T);
  template<class T> S(T t);
};
template<class U> struct W {
  template<class T> int f(T);
  template<class T, class R> R g(T*, R);
};
int main() {
  S s(5);
  W<int> w;
  W<char> wc;
  int x = 0;
  return s.v + s.get(1) + int(s.get(2L)) + S::st('c') + w.f('a') + int(wc.g(&x, 3L)) == 17 ? 0 : 1;
}
