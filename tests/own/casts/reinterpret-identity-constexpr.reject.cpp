// #199, [expr.const]: a reinterpret_cast to the operand's own type still prevents constant
// evaluation.
// XFAIL: gcc GCC 16 folds a reinterpret_cast to the operand's own type into the operand
constexpr int value = 4;
constexpr int same_value = reinterpret_cast<int>(value); //! error
constexpr const int* same_pointer = reinterpret_cast<const int*>(&value); //! error
constexpr int same_glvalue = reinterpret_cast<const int&>(value); //! error
