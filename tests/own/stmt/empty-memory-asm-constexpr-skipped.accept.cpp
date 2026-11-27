// [expr.const.core]/2
constexpr int value(bool checkpoint) {
  if (checkpoint) {
    asm volatile("" ::: "memory");
  }
  return 7;
}

static_assert(value(false) == 7);
