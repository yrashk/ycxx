// #297: function templates whose signatures hold pack-index-specifiers, mangled `Dy <type>
// <expression>` (Itanium C++ ABI §5.1.5) and a substitution candidate. The Clang peer only declares
// `at` and `pick`, whose index is a template parameter, where Clang and GCC agree, so its calls link
// against the specializations this object defines.
// PEER: pack-index-signatures-peer.cpp
// EXIT: 0
template<int I, class... T> T...[I] at(T... t) { return 7; }
template<int I, class... T> int pick(T...[I] x, T...[I]* p, T... t) { return x + (p ? 1 : 0); }

int peer();

int main() {
  if (at<1>(1.5, 2) != 7 || at<0>(3, 'c') != 7) return 1;
  int x = 4;
  if (pick<0, int, char>(4, &x, 1, 'a') != 5) return 2;
  return peer();
}
