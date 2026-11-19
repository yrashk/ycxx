// #133: operator functions as libycxx declares std::byte's ([over.oper], [over.match.oper]):
// non-member operators of a scoped enumeration found by argument-dependent lookup, compound
// assignment, unary ~; member operators of a class; an unscoped enumeration without operators
// keeps the built-in ones. noexcept-specifiers are accepted.
// EXIT: 0
namespace sb {
enum class byte : unsigned char {};
constexpr byte& operator|=(byte& l, byte r) noexcept { return l = byte(static_cast<unsigned int>(l) | static_cast<unsigned int>(r)); }
constexpr byte operator|(byte l, byte r) noexcept { return byte(static_cast<unsigned int>(l) | static_cast<unsigned int>(r)); }
constexpr byte operator&(byte l, byte r) noexcept { return byte(static_cast<unsigned int>(l) & static_cast<unsigned int>(r)); }
constexpr byte operator~(byte b) noexcept { return byte(~static_cast<unsigned int>(b)); }
}
struct V { int x, y; };


struct C { int n; int operator-() const { return -n; } C& operator+=(int k) { n += k; return *this; } };
enum E { e1 = 1, e2 = 2 };
int main() {
  sb::byte a{3}, b{5};
  sb::byte c = a | b;      // ADL finds sb::operator|
  c |= sb::byte{8};
  sb::byte d = ~a & c;
  int both = e1 | e2;      // built-in: no user operator for E
  C k{4};
  k += 3;
  return static_cast<int>(c) - 15 + static_cast<int>(d) - 12 + both - 3 + (-k) + 7;
}
