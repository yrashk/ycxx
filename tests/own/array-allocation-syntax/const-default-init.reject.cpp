// [expr.new], [dcl.init.general]: const scalar elements require initialization.
const int* uninitialized(unsigned n) { return new const int[n]; } //! error
