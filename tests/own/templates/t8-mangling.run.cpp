// ζ t8 (#88, #208): template template arguments and `__make_integer_seq` in function template
// specializations' names are Clang's (Itanium §5.1.5.7-8, with Clang's <template-param-decl>
// prefixes `Tn`, `Tp`, `Tt`, for packs too, and `TT<args>` with TT's defaults): the peer, compiled
// by Clang, defines them; this calls them.
// XFAIL: gcc GCC writes Tt for equivalent heads and no Tn, and has no __make_integer_seq
// PEER: t8-mangling-peer.cpp
// EXIT: 0
template<class T> struct Box { T v; };
template<class T, class U = int> struct V { T a; U b; };
template<class T, T... Is> struct iseq {};
namespace ns { template<class T> struct NBox {}; }
template<template<class> class TT> int f(TT<int>*);
template<template<class U, U...> class S> int g();
template<template<class> class TT> int h();
template<class T, T N> int k();
template<class T, T... Is> int count(iseq<T, Is...>*);
template<class T, T N> int m(__make_integer_seq<iseq, T, N>*);
template<class... Ts> struct Var {};
template<class T, class U> struct P2 {};
template<template<class> class... TTs> int p1();
template<template<class...> class... TTs> int p5();
template<int... Is> int q2();
template<template<class, class...> class TT> int d2(TT<char>*);
template<template<class, class = long> class TT> int d1(TT<char>*);
int packs() { return p1<>() + p1<V>() + p1<Box, V>() + p1<V, Box>() + p5<Box>() + q2<>() + d2<Var>(0) + d1<P2>(0); }
int main() {
  if (packs() != 1 + 1 + 1 + 1 + 2 + 3 + 4 + 5) return 2;
  return f((Box<int>*)0) + f((V<int>*)0) + g<iseq>() + h<ns::NBox>() + h<V>() + k<int, 3>() +
                 count((iseq<unsigned long, 0, 1, 2>*)0) + m<int, 2>(0) == 45 ? 0 : 1;
}
