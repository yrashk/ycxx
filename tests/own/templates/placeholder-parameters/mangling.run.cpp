// #1244: a function template specialization whose parameter is declared `auto` or
// `decltype(auto)` has Clang's `TnDa`/`TnDc` (and `TpTnDa` for a pack) before its argument in its
// name, as GCC 16 and Clang 23 write (Clang alone also for an empty pack): the peer, compiled by
// the oracle, defines them; this calls them.
// XFAIL: gcc GCC writes no TpTnDa before an empty pack (k<>)
// PEER: mangling-peer.cpp
// EXIT: 0
template<auto X> struct cw {};
template<auto N> int f(cw<N>);
template<auto N> int g();
template<decltype(auto) N> int h();
template<auto... N> int k();
template<class T, auto X> int w(T);
namespace pn { template<auto V> int scoped(); }
struct M { template<auto V> static int mem(); };
template<auto... V> struct pp {};
template<int V> struct ci {};
template<template<auto> class TT> int auto_head();
template<template<int> class TT> int int_head();
template<template<auto...> class TT> int pack_head();
int main() {
  if (f(cw<7>{}) + g<3>() + g<'c'>() + h<4u>() + k<1, 'a'>() + k<>() + w<int, 2>(1) != 7 + 3 + 99 + 4 + 2 + 0 + 3) return 1;
  // Scoped names, and template template parameters whose heads declare placeholders.
  return pn::scoped<4>() + M::mem<5>() + auto_head<cw>() + auto_head<ci>() + int_head<cw>() + pack_head<pp>() ==
                 4 + 5 + 6 + 6 + 7 + 8
             ? 0
             : 2;
}
