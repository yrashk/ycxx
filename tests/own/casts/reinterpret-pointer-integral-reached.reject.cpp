// #199, [expr.const]: evaluating a reinterpret_cast prevents constant evaluation.
int item;
constexpr auto bits = reinterpret_cast<unsigned long>(&item); //! error
constexpr auto pointer = reinterpret_cast<const void*>(1UL); //! error
