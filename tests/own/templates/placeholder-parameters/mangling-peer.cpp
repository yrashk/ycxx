// The peer of mangling.run.cpp, compiled by the oracle: the definitions, explicitly instantiated.
template<auto X> struct cw {};
template<auto N> int f(cw<N>) { return N; }
template<auto N> int g() { return N; }
template<decltype(auto) N> int h() { return N; }
template<auto... N> int k() { return sizeof...(N); }
template<class T, auto X> int w(T t) { return X + t; }
template int f<7>(cw<7>);
template int g<3>();
template int g<'c'>();
template int h<4u>();
template int k<1, 'a'>();
template int k<>();
template int w<int, 2>(int);
namespace pn { template<auto V> int scoped() { return V; } }
struct M { template<auto V> static int mem() { return V; } };
template<auto... V> struct pp {};
template<int V> struct ci {};
template<template<auto> class TT> int auto_head() { return 6; }
template<template<int> class TT> int int_head() { return 7; }
template<template<auto...> class TT> int pack_head() { return 8; }
template int pn::scoped<4>();
template int M::mem<5>();
template int auto_head<cw>();
template int auto_head<ci>();
template int int_head<cw>();
template int pack_head<pp>();
