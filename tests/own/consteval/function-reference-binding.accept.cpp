// #102, [expr.const.const], [expr.const.defns]: a reference variable can bind directly
// to an immediate function in its constant initializer.
// XFAIL: clang,gcc Clang 23 and GCC 16 reject this binding.
consteval int immediate() { return 42; }

int (&reference)() = (0, immediate);
