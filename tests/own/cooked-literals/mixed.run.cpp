// [over.literal], Itanium C++ ABI §5.1.5.1: a namespace literal operator can live in a peer object.
// PEER: inputs/provider.cpp
// EXIT: 0
namespace Units { unsigned long long operator""_k(unsigned long long); }
int main() {
  using Units::operator""_k;
  return 2_k != 2000;
}
