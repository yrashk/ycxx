// The other side of symbols.run.cpp, compiled by Clang.
template <class T> int tag();
int over(__int128*);
int over(const unsigned __int128*);
template <__int128 V> int value();
template <unsigned __int128 V> int uvalue();

int from_peer(__int128*, unsigned __int128*) { return 9; }

int peer() {
  if (tag<__int128>() != 1 || tag<unsigned __int128>() != 2 || tag<__int128*>() != 3) return 11;
  if (over((__int128*)nullptr) != 4 || over((const unsigned __int128*)nullptr) != 5) return 12;
  if (value<(__int128)1 << 100>() != 6 || value<-((__int128)1 << 90)>() != 7) return 13;
  if (uvalue<~(unsigned __int128)0>() != 8) return 14;
  return 0;
}
