// [expr.const.core]/2
static_assert([] { asm volatile("" ::: "memory"); return true; }()); //! error
