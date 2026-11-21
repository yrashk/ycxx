// #199, [expr.const]: a reached reinterpret_cast prevents constant evaluation
// even when its result is discarded.
// XFAIL: gcc GCC 16 accepts a discarded reinterpret_cast in a constant expression
static_assert((static_cast<void>(reinterpret_cast<const void*>(1UL)), true)); //! error
