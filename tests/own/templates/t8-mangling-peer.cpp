// The peer of t8-mangling.run.cpp, compiled by Clang: the definitions, explicitly instantiated.
template<class T> struct Box { T v; };
template<class T, class U = int> struct V { T a; U b; };
template<class T, T... Is> struct iseq {};
namespace ns { template<class T> struct NBox {}; }
template<template<class> class TT> int f(TT<int>*) { return 1; }
template<template<class U, U...> class S> int g() { return 3; }
template<template<class> class TT> int h() { return 7; }
template<class T, T N> int k() { return int(N); }
template<class T, T... Is> int count(iseq<T, Is...>*) { return sizeof...(Is); }
template<class T, T N> int m(__make_integer_seq<iseq, T, N>*) { return int(N) * 10; }
template<class... Ts> struct Var {};
template<class T, class U> struct P2 {};
template<template<class> class... TTs> int p1() { return 1; }
template<template<class...> class... TTs> int p5() { return 2; }
template<int... Is> int q2() { return 3; }
template<template<class, class...> class TT> int d2(TT<char>*) { return 4; }
template<template<class, class = long> class TT> int d1(TT<char>*) { return 5; }
template int p1<>();
template int p1<V>();
template int p1<Box, V>();
template int p1<V, Box>();
template int p5<Box>();
template int q2<>();
template int d2<Var>(Var<char>*);
template int d1<P2>(P2<char, long>*);
template int f<Box>(Box<int>*);
template int f<V>(V<int>*);
template int g<iseq>();
template int h<ns::NBox>();
template int h<V>();
template int k<int, 3>();
template int count<unsigned long, 0, 1, 2>(iseq<unsigned long, 0, 1, 2>*);
template int m<int, 2>(iseq<int, 0, 1>*);
