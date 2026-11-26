// [expr.const.core]/2. Keep the asm and required invocation on one diagnostic line.
constexpr bool checkpoint(bool run) { if (run) asm volatile("" ::: "memory"); return true; } static_assert(checkpoint(true)); //! error
