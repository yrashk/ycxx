// [dcl.enum], [expr.static.cast]: check the original value before conversion to storage width.
// XFAIL: gcc GCC 16 accepts out-of-range enum casts in constant evaluation
enum Ordinary { largest = 5 };
constexpr Ordinary as_ordinary(unsigned long long value) { return static_cast<Ordinary>(value); }

constexpr Ordinary literal = static_cast<Ordinary>(8); //! error
constexpr Ordinary via_call = as_ordinary(0x1'0000'0000ULL); //! error

enum Signed { negative = -3, positive = 2 };
constexpr Signed below = static_cast<Signed>(-5); //! error
