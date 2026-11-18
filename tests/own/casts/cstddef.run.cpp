// #133: what libycxx's <cstddef> needs: decltype ([dcl.type.decltype]) of an id-expression, a
// member access, a parenthesized lvalue and a prvalue; sizeof and alignof of fundamental, array
// and enumeration types and of an expression ([expr.sizeof], [expr.alignof]); static_cast,
// functional and C-style casts between integers, enumerations and pointers to void
// ([expr.static.cast], [expr.type.conv], [expr.cast]); objects of type std::nullptr_t.
// EXIT: 0
typedef decltype(sizeof(0)) size_type;
typedef decltype(static_cast<int*>(nullptr) - static_cast<int*>(nullptr)) diff_type;
typedef decltype(nullptr) null_type;
enum class byte : unsigned char {};
enum Color { red, green };
struct P { int x; long y; };
int g = 5;
decltype(g) dg = 1;
struct M { int v; } mo;
int main() {
  int& gr = g;
  decltype(gr) dr = g;
  decltype((g)) dp = g;
  decltype(mo.v) mv = 6;
  size_type s = sizeof(int) + sizeof(long) + sizeof(char[3]) + alignof(long) + sizeof g + sizeof(byte);  // 4+8+3+8+4+1 = 28
  diff_type d = -1;
  null_type n = nullptr;
  byte b = byte(200);
  unsigned u = static_cast<unsigned>(b) + static_cast<unsigned int>(byte{3});  // 203
  Color c = static_cast<Color>(1);
  int i = int(3L) + (int)4L + int{5} + (int)c;  // 13
  void* vp = &g;
  int* ip = static_cast<int*>(vp);
  (void)n;
  static_cast<void>(d);
  bool flag = bool(2);
  return int(s) - 28 + int(u) - 203 + i - 13 + *ip - 5 + dg - 1 + (&dr == &g ? 0 : 1) + (&dp == &g ? 0 : 1) + flag - 1 + mv - 6;
}
