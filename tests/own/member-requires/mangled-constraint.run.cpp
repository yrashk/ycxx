// #1417 review: the name of a non-template member with a trailing requires-clause encodes the
// clause, as Clang and GCC do (Itanium C++ ABI §5.1.2), so its static local is one object across
// objects compiled by different compilers.
// PEER: mangled-constraint-peer.cpp
#include "mangled-constraint.hpp"
int peer_next();
int main() {
  S<int>{}.next();
  peer_next();
  return S<int>{}.next() == 3 ? 0 : 1;
}
