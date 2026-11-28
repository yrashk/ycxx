// #199, [expr.reinterpret.cast]/8, [expr.const.cast]/7: converting a pointer to const void to a
// function pointer casts away the pointee's const.
// XFAIL: gcc GCC 16 accepts the conversion (#1702)
using Callable = int (*)(int);
Callable from_code(const void* code) { return reinterpret_cast<Callable>(code); } //! error
Callable from_mutable(void* code) { return reinterpret_cast<Callable>(code); }
