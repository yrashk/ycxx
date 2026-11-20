// [lex.ext], [basic.lookup.unqual]: ordinary lookup includes a block using-declaration, a
// using-directive, and the enclosing namespace. An identifier can share the suffix's spelling.
// EXIT: 0
namespace A { constexpr int operator""_k(unsigned long long) { return 7; } }
namespace B { constexpr int operator""_k(unsigned long long) { return 11; } }
constexpr int u() {
  using A::operator""_k;
  return 2_k;
}
namespace B { constexpr int v() { return 2_k; } }
int main() {
  using namespace A;
  int _k = 0;
  return u() + B::v() + 2_k + _k - 25;
}
