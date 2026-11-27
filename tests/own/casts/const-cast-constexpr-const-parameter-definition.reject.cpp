// XFAIL: clang Clang follows the prior declaration instead of the definition parameter cv
// XFAIL: gcc GCC accepts a constexpr write to a const by-value class parameter
// [dcl.fct]/5, [dcl.type.cv]/4, [expr.const.core]
struct Cell { int data; };
constexpr int change(Cell);
constexpr int change(const Cell copy) {
  const_cast<int&>(copy.data) = 17;
  return copy.data;
}
static_assert(change(Cell{5}) == 17); //! error
