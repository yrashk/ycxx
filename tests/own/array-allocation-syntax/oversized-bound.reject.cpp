// [expr.new]: a representable element count can still exceed the byte limit.
int* oversized() { return new int[1UL << 62]; } //! error
