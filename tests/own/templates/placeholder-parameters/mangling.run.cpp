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
int main() {
  return f(cw<7>{}) + g<3>() + g<'c'>() + h<4u>() + k<1, 'a'>() + k<>() + w<int, 2>(1) == 7 + 3 + 99 + 4 + 2 + 0 + 3 ? 0 : 1;
}
