// [class.temporary], [expr.const.core] A selected reference-argument temporary expires
// at the end of the return operand's full-expression.
constexpr const int& hold(const int& value) { return value; }
constexpr const int* escaped(bool choose) { return choose ? &hold(42) : nullptr; }
static_assert(escaped(false) == nullptr);
static_assert(*escaped(true) == 42); //! error
