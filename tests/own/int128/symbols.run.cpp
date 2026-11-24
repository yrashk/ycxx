// #1172: __int128 and unsigned __int128 in symbols, `n` and `o` (Itanium C++ ABI §5.1.5.2), and
// a template argument of type __int128 beyond 64 bits as `L n <value> E` (§5.1.5.7). The Clang
// peer declares what this object defines and defines what it declares. No 128-bit value crosses
// the boundary, only pointers and int results.
// FLAGS: -Wno-pedantic
// PEER: symbols-peer.cpp
template <class T> int tag();
template <> int tag<__int128>() { return 1; }
template <> int tag<unsigned __int128>() { return 2; }
template <> int tag<__int128_t*>() { return 3; }
int over(__int128*) { return 4; }
int over(const unsigned __int128*) { return 5; }
template <__int128 V> int value();
template <> int value<(__int128)1 << 100>() { return 6; }
template <> int value<-((__int128)1 << 90)>() { return 7; }
template <unsigned __int128 V> int uvalue();
template <> int uvalue<~(unsigned __int128)0>() { return 8; }
template <class T> struct P;
template <> struct P<__int128> { int v = 1; };
template <> struct P<unsigned __int128> { int v = 2; };

int from_peer(__int128*, unsigned __int128*);
int peer();

int main() {
  if (tag<__int128>() + tag<unsigned __int128>() != 3 || P<unsigned __int128>{}.v != 2) return 1;
  if (from_peer(nullptr, nullptr) != 9) return 2;
  return peer();
}
