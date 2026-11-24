// [expr.const]: evaluating __builtin_unreachable() is undefined behavior, so an expression that
// reaches it is not a core constant expression; one that does not reach it can be.
constexpr int checked(int x) {
  if (x >= 0) return x;
  __builtin_unreachable();
}
static_assert(checked(4) == 4);
static_assert((true ? 37 : (__builtin_unreachable(), 91)) == 37);

static_assert((false ? 37 : (__builtin_unreachable(), 91)) == 91); //! error
// GCC reports a call's failure where the builtin is reached, Clang at the call: one line for both.
constexpr int reaching(int x) { if (x >= 0) return x; __builtin_unreachable(); } constexpr int reached = reaching(-1); //! error

void one_argument() { __builtin_unreachable(0); } //! error
