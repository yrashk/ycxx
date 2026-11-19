// [expr.unary.op], [expr.pre.incr], [expr.ass]: unary + and - on a floating operand keep its type
// (only integral operands are promoted), and so do ++, -- and assignment; ! gives bool.
constexpr int kind(float) { return 1; }
constexpr int kind(double) { return 2; }
constexpr int kind(long double) { return 3; }
constexpr int kind(bool) { return 4; }
constexpr int kind(int) { return 5; }
constexpr float f = 1.5f;
static_assert(kind(+f) == 1 && kind(-f) == 1);
static_assert(kind(+1.0) == 2 && kind(-1.0L) == 3);
static_assert(kind(!f) == 4);
static_assert(kind(+'a') == 5); // for contrast: char is promoted
static_assert(-f == -1.5f && +f == 1.5f);
constexpr int k() {
  float x = 0;
  double y = 0;
  return kind(++x) * 10 + kind(y = 2.5f);
}
static_assert(k() == 12);
