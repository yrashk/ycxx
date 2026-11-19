// ζ t8 (#88, #208): template template parameters ([temp.param]/1) and their arguments
// ([temp.arg.template]): a class template whose head fits the parameter's, with defaults and packs
// (P0522); a default argument; a pack of template template parameters; `TT<args>` as a type.
// EXIT: 0
template<class T> struct Box { T v; using type = T; };
template<class T, class U = int> struct V { T a; U b; using type = U; };
template<class T, T... Is> struct iseq { static constexpr int n = sizeof...(Is); };
template<class T, int... Is> struct intseq { static constexpr int n = sizeof...(Is) + 10; };
template<template<class> class TT> struct W { TT<int> x; int get() { return x.v; } };
template<template<class> class TT = Box> struct D { TT<char> c; };
template<template<class> class... TTs> struct P { static constexpr int n = sizeof...(TTs); };
template<template<class> class TT, class T> struct Q { typename TT<T>::type t; };
template<template<class U, U...> class S> int seq() { return S<int, 1, 2>::n; }
template<template<class...> class TT> int any() { TT<int> x; x.v = 1; return x.v; }
int main() {
  W<Box> w;
  w.x.v = 5;
  D<> d;
  d.c.v = 'a';
  Q<V, char> q;
  q.t = 3;
  if (w.get() != 5 || d.c.v != 'a' || q.t != 3) return 1;
  if (P<Box, V>::n != 2 || P<>::n != 0) return 2;
  if (seq<iseq>() != 2 || seq<intseq>() != 12) return 3;
  return any<Box>() == 1 ? 0 : 4;
}
