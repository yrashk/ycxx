// [class.temporary], [stmt.if], [expr.const.core] The selected reference-argument
// temporary expires at the end of the condition's full-expression, before the read.
constexpr bool retain(const int*& saved, const int& value) {
  saved = &value;
  return true;
}

constexpr int after_condition(bool reached) {
  const int* saved = nullptr;
  if (!(reached && retain(saved, 7))) return 42;
  return *saved; } static_assert(after_condition(true) == 7); //! error

static_assert(after_condition(false) == 42);
