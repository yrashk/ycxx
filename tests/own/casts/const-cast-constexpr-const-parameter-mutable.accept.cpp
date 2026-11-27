// [dcl.type.cv]/4, [expr.const.core]
struct Cell { int data; mutable int memo; };
constexpr int memoize(const Cell copy) {
  const int& view = copy.memo;
  const_cast<int&>(view) = 9;
  return copy.data + copy.memo;
}
static_assert(memoize(Cell{5, 0}) == 14);
