// XFAIL: clang Clang follows the prior declaration instead of the definition parameter cv
// [dcl.fct]/5, [dcl.type.cv]/4, [expr.const.core]
struct Cell { int data; };
constexpr int change(const Cell);
constexpr int change(Cell copy) {
  const Cell& view = copy;
  const_cast<int&>(view.data) = 17;
  return copy.data;
}
static_assert(change(Cell{5}) == 17);
